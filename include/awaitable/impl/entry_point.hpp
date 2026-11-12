#pragma once

#include <exception>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

template <class T> struct spawn_value {
    using type = typename std::conditional<std::is_default_constructible<T>::value, T,
                                           aw::optional<T>>::type;
};
template <class T> using spawn_value_t = typename spawn_value<T>::type;

template <class T> struct spawn_signature { using type = void(std::exception_ptr, spawn_value_t<T>); };
template <> struct spawn_signature<void> { using type = void(std::exception_ptr); };

template <class T> struct spawn_result {
    using value_type = spawn_value_t<T>;
    aw::optional<T> value;
    template <class F> void capture(F&& get) { value.emplace(get()); }
    value_type take() {
        if (value) return value_type(std::move(*value));
        return value_type();
    }
};

template <class Handler, class V> struct deliver_op {
    Handler handler;
    std::exception_ptr e;
    V v;
    void operator()() { handler(e, std::move(v)); }
};
template <class Handler> struct deliver_void_op {
    Handler handler;
    std::exception_ptr e;
    void operator()() { handler(e); }
};

// §7.2：手写入口帧。状态机：开始（压入用户帧）→ 运行中 → [启动期完成：先 post] → 交付。
template <class Executor, class T, class Handler>
class entry_frame_impl final : public entry_frame_base<Executor> {
    using base = entry_frame_base<Executor>;

  public:
    template <class H>
    static entry_frame_impl* create(Executor const& ex, awaitable<T, Executor> user, H&& handler) {
        void* const memory = recycling_allocate(mem_tag::awaitable_frame, sizeof(entry_frame_impl),
                                                alignof(entry_frame_impl));
        try {
            return ::new (memory) entry_frame_impl(ex, std::move(user), std::forward<H>(handler));
        } catch (...) {
            recycling_deallocate(mem_tag::awaitable_frame, memory, sizeof(entry_frame_impl));
            throw;
        }
    }

  private:
    enum class state : unsigned char { start, running, deliver };

    template <class H>
    entry_frame_impl(Executor const& ex, awaitable<T, Executor> user, H&& handler)
        : base{ex}, user_{std::move(user)}, handler_(std::forward<H>(handler)) {
        this->block_.header = co2::detail::FrameHeader{&destroy_entry, &step_entry, false, false};
    }
    ~entry_frame_impl() = default;

    static entry_frame_impl* from(co2::detail::FrameHeader* header) noexcept {
        return static_cast<entry_frame_impl*>(
            reinterpret_cast<typename base::header_block*>(header)->self);
    }

    static void destroy_entry(co2::detail::FrameHeader* header) noexcept {
        entry_frame_impl* const self = from(header);
        self->~entry_frame_impl();
        recycling_deallocate(mem_tag::awaitable_frame, self, sizeof(entry_frame_impl));
    }

    static co2::detail::FrameHeader* step_entry(co2::detail::FrameHeader* header) {
        from(header)->step();
        return nullptr;
    }

    void step() {
        switch (state_) {
        case state::start:
            CO2_CONTRACT_CHECK(user_.valid());
            state_ = state::running;
            user_.frame_->push_frame(this);
            return;
        case state::running:
            if (launching_marker() == static_cast<void const*>(static_cast<base*>(this))) {
                state_ = state::deliver;
                this->after_suspend(&reschedule_via_post, this);
                return;
            }
            deliver();
            return;
        case state::deliver:
            deliver();
            return;
        }
    }

    // §7.3 第 4 条：启动期同步完成，先 post 回 executor，再交付。
    static void reschedule_via_post(void* arg) {
        entry_frame_impl* const self = static_cast<entry_frame_impl*>(arg);
        awaitable_thread<Executor>* const thread = self->detach_thread();
        Executor ex = self->executor_;
        ex.post(resume_thread_op<Executor>{awaitable_thread<Executor>{std::move(*thread)}});
    }

    void deliver() { deliver(std::is_void<T>{}); }

    void deliver(std::false_type) {
        std::exception_ptr e;
        spawn_result<T> result;
        {
            awaitable<T, Executor> done{std::move(user_)};
            try {
                result.capture([&]() -> T { return done.frame_->get(); });
            } catch (...) {
                e = std::current_exception();
            }
        } // 用户帧在交付前归还缓存
        using op_type = deliver_op<Handler, spawn_value_t<T>>;
        dispatch_to_handler(op_type{std::move(handler_), e, result.take()});
    }

    void deliver(std::true_type) {
        std::exception_ptr e;
        {
            awaitable<T, Executor> done{std::move(user_)};
            try {
                done.frame_->get();
            } catch (...) {
                e = std::current_exception();
            }
        }
        dispatch_to_handler(deliver_void_op<Handler>{std::move(handler_), e});
    }

    // §7.3 第 5 条：交付到 token 的关联 executor；回调抛出的异常经 pump 的 complete 重抛。
    template <class Op> void dispatch_to_handler(Op&& op) {
        try {
            auto target = get_associated_executor(op.handler, this->executor_);
            target.dispatch(std::forward<Op>(op));
        } catch (...) {
            this->pending_exception_ = std::current_exception();
        }
        this->pop_frame();
    }

    awaitable<T, Executor> user_;
    Handler handler_;
    state state_{state::start};
};

template <class Executor> struct launch_op {
    awaitable_thread<Executor> thread;
    void operator()() { thread.launch(); }
};

template <class Executor> struct initiate_co_spawn {
    Executor ex;

    template <class Handler, class T> void operator()(Handler&& handler, awaitable<T, Executor> a) const {
        using handler_type = typename std::decay<Handler>::type;
        auto* const entry = entry_frame_impl<Executor, T, handler_type>::create(
            ex, std::move(a), std::forward<Handler>(handler));
        awaitable_thread<Executor> thread{entry};
        // §7.3 第 3 条：先 dispatch 到 ex，再 launch。
        ex.dispatch(launch_op<Executor>{std::move(thread)});
    }
};

} // namespace detail
} // namespace aw
