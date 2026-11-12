#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/impl/entry_point.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

// §12 第 25、26 行：两个操作数各开一个子执行线程，加一个手写组状态（mem_tag::parallel_group）。
enum class group_condition : unsigned char {
    one_error,   // wait_for_one_error：有一个失败就取消另一个（operator&&）
    one_success, // wait_for_one_success：有一个成功就取消另一个（operator||）
};

struct no_value {};

template <class T> struct child_value { using type = aw::optional<spawn_value_t<T>>; };
template <> struct child_value<void> { using type = no_value; };
template <class T> using child_value_t = typename child_value<T>::type;

// co_spawn 交付 spawn_value_t<T>；T 不可默认构造时那是 optional<T>，这里剥掉一层。
template <class T> T&& unwrap_child(aw::optional<T>& v) { return std::move(*v); }
template <class T> T&& unwrap_child(aw::optional<aw::optional<T>>& v) { return std::move(**v); }

// 组的完成值：先完成者的下标、两个异常、两个值。
template <class T, class U> struct pair_outcome {
    unsigned first{0U};
    std::exception_ptr e0{};
    std::exception_ptr e1{};
    child_value_t<T> v0{};
    child_value_t<U> v1{};
};

template <class Executor, class T, class U, class Handler> class pair_group_state {
  public:
    using outcome_type = pair_outcome<T, U>;

    template <class H> static pair_group_state* create(H&& handler, Executor const& ex, group_condition cond) {
        void* const memory = recycling_allocate(mem_tag::parallel_group, sizeof(pair_group_state),
                                                alignof(pair_group_state));
        try {
            return ::new (memory) pair_group_state(std::forward<H>(handler), ex, cond);
        } catch (...) {
            recycling_deallocate(mem_tag::parallel_group, memory, sizeof(pair_group_state));
            throw;
        }
    }

    // 在执行线程的 executor 上调用（initiation 里）。
    void start(awaitable<T, Executor> t, awaitable<U, Executor> u) {
        co_spawn(ex_, std::move(t), bind_cancellation_slot(signals_[0].slot(), child_done<0U, T>{this}));
        co_spawn(ex_, std::move(u), bind_cancellation_slot(signals_[1].slot(), child_done<1U, U>{this}));
        bool emit_now = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            spawned_ = true;
            emit_now = cancel_pending_;
        }
        if (emit_now) emit_others(pending_target_);
        // §6.3：父执行线程收到的取消转给两个子执行线程。
        parent_slot_ = get_associated_cancellation_slot(handler_);
        if (parent_slot_.is_connected()) parent_slot_.template emplace<forward_cancel>(forward_cancel{this});
        finish_one(); // initiation 自己持有的那一份
    }

  private:
    pair_group_state(Handler&& handler, Executor const& ex, group_condition cond)
        : handler_(std::move(handler)), ex_{ex}, cond_{cond} {}

    struct forward_cancel {
        pair_group_state* self;
        void operator()(cancellation_type type) {
            self->signals_[0].emit(type);
            self->signals_[1].emit(type);
        }
    };

    template <unsigned I, class V> struct child_done {
        pair_group_state* self;
        template <class... Vs> void operator()(std::exception_ptr e, Vs&&... v) {
            self->store(std::integral_constant<unsigned, I>{}, std::move(e), std::forward<Vs>(v)...);
            self->decide(I, I == 0U ? !!self->outcome_.e0 : !!self->outcome_.e1);
            self->finish_one();
        }
    };

    void store(std::integral_constant<unsigned, 0U>, std::exception_ptr e) { outcome_.e0 = std::move(e); }
    void store(std::integral_constant<unsigned, 1U>, std::exception_ptr e) { outcome_.e1 = std::move(e); }
    template <class V> void store(std::integral_constant<unsigned, 0U>, std::exception_ptr e, V&& v) {
        outcome_.e0 = std::move(e);
        if (!outcome_.e0) outcome_.v0.emplace(std::forward<V>(v));
    }
    template <class V> void store(std::integral_constant<unsigned, 1U>, std::exception_ptr e, V&& v) {
        outcome_.e1 = std::move(e);
        if (!outcome_.e1) outcome_.v1.emplace(std::forward<V>(v));
    }

    // 第一个满足条件的完成者取消另一个（cancellation_type::all，asio cancellation_condition.hpp:41-69）。
    void decide(unsigned index, bool failed) {
        bool const trigger = cond_ == group_condition::one_error ? failed : !failed;
        bool emit_now = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (completed_++ == 0U) outcome_.first = index;
            if (trigger && !cancel_claimed_) {
                cancel_claimed_ = true;
                if (spawned_) {
                    emit_now = true;
                } else {
                    cancel_pending_ = true;
                    pending_target_ = index;
                }
            }
        }
        if (emit_now) emit_others(index);
    }

    void emit_others(unsigned winner) { signals_[winner == 0U ? 1U : 0U].emit(cancellation_type::all); }

    void finish_one() {
        if (remaining_.fetch_sub(1U, std::memory_order_acq_rel) != 1U) return;
        // 经回调的关联 executor 交付（§8.1 操作作者契约第 2 条）。
        auto target = get_associated_executor(handler_, ex_);
        target.dispatch(deliver{this});
    }

    struct deliver {
        pair_group_state* self;
        void operator()() {
            if (self->parent_slot_.is_connected()) self->parent_slot_.clear();
            Handler handler(std::move(self->handler_));
            outcome_type outcome(std::move(self->outcome_));
            self->~pair_group_state();
            recycling_deallocate(mem_tag::parallel_group, self, sizeof(pair_group_state));
            handler(std::move(outcome));
        }
    };

    Handler handler_;
    Executor ex_;
    group_condition cond_;
    cancellation_signal signals_[2];
    cancellation_slot parent_slot_{};
    outcome_type outcome_{};
    std::atomic<unsigned> remaining_{3U}; // 两个子执行线程 + initiation
    std::mutex mutex_;
    unsigned completed_{0U};
    bool spawned_{false};
    bool cancel_claimed_{false};
    bool cancel_pending_{false};
    unsigned pending_target_{0U};
};

template <class Executor> struct initiate_pair_group {
    group_condition cond;
    template <class Handler, class T, class U>
    void operator()(Handler&& handler, awaitable<T, Executor> t, awaitable<U, Executor> u) const {
        using handler_type = typename std::decay<Handler>::type;
        using state_type = pair_group_state<Executor, T, U, handler_type>;
        Executor const ex = handler.get_executor(); // 只配合 use_awaitable：回调就是执行线程
        state_type* const state = state_type::create(std::forward<Handler>(handler), ex, cond);
        state->start(std::move(t), std::move(u));
    }
};

template <class Executor, class T, class U, class Token>
auto async_pair_group(group_condition cond, awaitable<T, Executor> t, awaitable<U, Executor> u, Token&& token) {
    return async_initiate<Token, void(pair_outcome<T, U>)>(initiate_pair_group<Executor>{cond}, token,
                                                           std::move(t), std::move(u));
}

} // namespace detail
} // namespace aw
