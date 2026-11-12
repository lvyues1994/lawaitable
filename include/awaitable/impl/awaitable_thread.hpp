#pragma once

#include <exception>
#include <utility>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/impl/awaitable_frame.hpp"

namespace aw {
namespace detail {

// 入口帧（栈底）。帧头与 core 兼容，可被 pump 恢复；状态机由 entry_frame_impl 实现。
template <class Executor> class entry_frame_base : public awaitable_frame_base<Executor> {
  public:
    struct header_block {
        co2::detail::FrameHeader header;
        entry_frame_base* self;
    };

    explicit entry_frame_base(Executor const& ex) : executor_{ex} {
        block_.self = this;
        this->coro_ = co2::coroutine_handle<>::from_address(&block_.header);
        top_of_stack_ = this;
        parent_slot_ = internal_signal_.slot();
        cancellation_state_ = cancellation_state{parent_slot_};
    }
    entry_frame_base(entry_frame_base const&) = delete;
    entry_frame_base& operator=(entry_frame_base const&) = delete;
    ~entry_frame_base() {
        // 先拆掉父 slot 上的状态处理器（它的观察者指向 stop_bridge），再释放桥。
        parent_slot_.clear();
        if (stop_bridge_ != nullptr) stop_bridge::destroy(stop_bridge_);
    }

    // co_spawn 的 token 带关联 slot 时，父 slot 改为引用计数桥里的 signal（§8.2）。
    void use_parent_slot(cancellation_slot parent) {
        parent_slot_.clear();
        parent_slot_ = parent;
        cancellation_state_ = cancellation_state{parent_slot_};
    }

    template <class In, class Out> void reset_cancellation_state(In in, Out out) {
        cancellation_state_ = cancellation_state{parent_slot_, in, out};
        if (stop_bridge_ != nullptr) stop_bridge_->attach(cancellation_state_);
    }

    // §6.4：第一次取 stop_token 时惰性创建桥。
    stop_bridge& bridge() {
        if (stop_bridge_ == nullptr) {
            stop_bridge_ = stop_bridge::create();
            stop_bridge_->attach(cancellation_state_);
        }
        return *stop_bridge_;
    }

    header_block block_{};
    Executor executor_;
    awaitable_frame_base<Executor>* top_of_stack_{nullptr};
    cancellation_signal internal_signal_;
    cancellation_slot parent_slot_;
    cancellation_state cancellation_state_;
    bool throw_if_cancelled_{true};
    stop_bridge* stop_bridge_{nullptr};
};

// §4.4：执行线程。只持有栈底；所有权在 pump 和完成回调之间转移。
template <class Executor> class awaitable_thread {
  public:
    using executor_type = Executor;

    explicit awaitable_thread(entry_frame_base<Executor>* entry) noexcept : bottom_of_stack_{entry} {}
    awaitable_thread(awaitable_thread&& other) noexcept : bottom_of_stack_{other.bottom_of_stack_} {
        other.bottom_of_stack_ = nullptr;
    }
    awaitable_thread(awaitable_thread const&) = delete;
    awaitable_thread& operator=(awaitable_thread const&) = delete;
    awaitable_thread& operator=(awaitable_thread&&) = delete;
    ~awaitable_thread() {
        if (bottom_of_stack_ != nullptr) unwind_through_executor();
    }

    executor_type get_executor() const noexcept { return bottom_of_stack_->executor_; }
    cancellation_slot get_cancellation_slot() const noexcept {
        return bottom_of_stack_->cancellation_state_.slot();
    }
    entry_frame_base<Executor>* entry_point() const noexcept { return bottom_of_stack_; }
    awaitable_frame_base<Executor>* frame() const noexcept { return bottom_of_stack_->top_of_stack_; }

    // asio :753-757 与 impl/awaitable.ipp:31-43：在“启动中”标记下 pump。
    void launch() {
        launching_scope scope{static_cast<void const*>(bottom_of_stack_)};
        frame()->attach_thread(this);
        pump();
    }

    // asio :778-786。
    void pump() {
        do {
            bottom_of_stack_->top_of_stack_->resume();
        } while (bottom_of_stack_ != nullptr && bottom_of_stack_->top_of_stack_ != nullptr);
        if (bottom_of_stack_ != nullptr) complete();
    }

  private:
    // asio :794-799：销毁入口帧；交付回调时抛出的异常在这里重抛（§7.5）。
    void complete() {
        entry_frame_base<Executor>* const entry = bottom_of_stack_;
        bottom_of_stack_ = nullptr;
        std::exception_ptr e = std::move(entry->pending_exception_);
        entry->destroy();
        if (e) std::rethrow_exception(e);
    }

    struct entry_owner {
        entry_frame_base<Executor>* entry;
        explicit entry_owner(entry_frame_base<Executor>* e) noexcept : entry{e} {}
        entry_owner(entry_owner&& other) noexcept : entry{other.entry} { other.entry = nullptr; }
        entry_owner(entry_owner const&) = delete;
        entry_owner& operator=(entry_owner const&) = delete;
        ~entry_owner() {
            if (entry != nullptr) {
                unwinding_scope scope;
                entry->destroy();
            }
        }
        void operator()() noexcept {}
    };

    // asio :763-774：被丢弃的执行线程经 executor 展开（关闭期间 post 立即销毁，即内联展开）。
    void unwind_through_executor() noexcept {
        entry_frame_base<Executor>* const entry = bottom_of_stack_;
        bottom_of_stack_ = nullptr;
        Executor ex = entry->executor_;
        ex.post(entry_owner{entry});
    }

    entry_frame_base<Executor>* bottom_of_stack_;
};

// 投递一个执行线程：在 executor 上重新 attach 并 pump。
template <class Executor> struct resume_thread_op {
    awaitable_thread<Executor> thread;
    void operator()() {
        thread.frame()->attach_thread(&thread);
        thread.pump();
    }
};

} // namespace detail
} // namespace aw
