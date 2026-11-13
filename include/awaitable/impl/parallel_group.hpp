#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/impl/entry_point.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

// §12 第 25、26 行：两个操作数各开一个子执行线程，加一个手写组状态（mem_tag::parallel_group）。
// 验收修订第 2 项：两个子执行线程的入口帧嵌在组状态这一块里，子执行线程的父 slot 直接是
// signals_[i]（不经 co_spawn 的取消桥）；向子执行线程 emit 时照旧先 dispatch 到 ex_（§6.2 第 7 条）。
// 组状态按引用计数释放：两个入口帧各一份，交付一份，每个在途的 emit 操作一份。
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
        launch_child<0U, T>(&child0_, std::move(t));
        launch_child<1U, U>(&child1_, std::move(u));
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

    // 嵌入的入口帧销毁时调用（owned_entry_storage）。
    void release_child() noexcept { release(); }

  private:
    pair_group_state(Handler&& handler, Executor const& ex, group_condition cond)
        : handler_(std::move(handler)), ex_{ex}, cond_{cond} {}

    void release() noexcept {
        if (refs_.fetch_sub(1U, std::memory_order_acq_rel) != 1U) return;
        this->~pair_group_state();
        recycling_deallocate(mem_tag::parallel_group, this, sizeof(pair_group_state));
    }

    struct forward_cancel {
        pair_group_state* self;
        void operator()(cancellation_type type) {
            self->emit_child(0U, type);
            self->emit_child(1U, type);
        }
    };

    // 在子执行线程的 executor 上 emit（原先由 co_spawn 的 spawn_cancellation_handler 完成）。
    struct emit_op {
        pair_group_state* self;
        unsigned index;
        cancellation_type type;
        emit_op(pair_group_state* s, unsigned i, cancellation_type t) noexcept : self{s}, index{i}, type{t} {}
        emit_op(emit_op&& other) noexcept : self{other.self}, index{other.index}, type{other.type} {
            other.self = nullptr;
        }
        emit_op(emit_op const&) = delete;
        emit_op& operator=(emit_op const&) = delete;
        ~emit_op() {
            if (self != nullptr) self->release();
        }
        void operator()() { self->signals_[index].emit(type); }
    };

    void emit_child(unsigned index, cancellation_type type) {
        refs_.fetch_add(1U, std::memory_order_relaxed);
        ex_.dispatch(emit_op{this, index, type});
    }

    template <unsigned I, class V> struct child_done {
        pair_group_state* self;
        template <class... Vs> void operator()(std::exception_ptr e, Vs&&... v) {
            self->store(std::integral_constant<unsigned, I>{}, take_exception(e), std::forward<Vs>(v)...);
            self->decide(I, I == 0U ? !!self->outcome_.e0 : !!self->outcome_.e1);
            self->finish_one();
        }
    };

    using storage_type = owned_entry_storage<pair_group_state>;
    template <unsigned I, class V>
    using child_entry = entry_frame_impl<Executor, V, child_done<I, V>, storage_type>;

    // §7.3 第 3 条：与 co_spawn 相同，先 dispatch 到 ex_，再 launch。
    template <unsigned I, class V, class Memory> void launch_child(Memory* memory, awaitable<V, Executor> a) {
        auto* const entry = child_entry<I, V>::construct_at(memory, storage_type{this}, ex_, signals_[I].slot(),
                                                            std::move(a), child_done<I, V>{this});
        awaitable_thread<Executor> thread{entry};
        ex_.dispatch(launch_op<Executor>{std::move(thread)});
    }

    void store(std::integral_constant<unsigned, 0U>, std::exception_ptr e) { outcome_.e0 = take_exception(e); }
    void store(std::integral_constant<unsigned, 1U>, std::exception_ptr e) { outcome_.e1 = take_exception(e); }
    template <class V> void store(std::integral_constant<unsigned, 0U>, std::exception_ptr e, V&& v) {
        outcome_.e0 = take_exception(e);
        if (!outcome_.e0) outcome_.v0.emplace(std::forward<V>(v));
    }
    template <class V> void store(std::integral_constant<unsigned, 1U>, std::exception_ptr e, V&& v) {
        outcome_.e1 = take_exception(e);
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

    void emit_others(unsigned winner) { emit_child(winner == 0U ? 1U : 0U, cancellation_type::all); }

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
            self->release(); // 交付的那一份；入口帧可能还没销毁
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
    std::atomic<unsigned> refs_{3U};      // 两个入口帧 + 交付 + 在途 emit
    std::mutex mutex_;
    unsigned completed_{0U};
    bool spawned_{false};
    bool cancel_claimed_{false};
    bool cancel_pending_{false};
    unsigned pending_target_{0U};
    alignas(child_entry<0U, T>) unsigned char child0_[sizeof(child_entry<0U, T>)];
    alignas(child_entry<1U, U>) unsigned char child1_[sizeof(child_entry<1U, U>)];
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
