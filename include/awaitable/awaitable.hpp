#pragma once

#include <utility>

#include "awaitable/core/dsl.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/thread_pool.hpp"
#include "awaitable/impl/awaitable_frame.hpp"

namespace aw {

// §4.1：只能移动；只能在同一 Executor 的 awaitable 帧里等待（await_transform 保证）。
template <class T, class Executor> class awaitable {
  public:
    using value_type = T;
    using executor_type = Executor;
    using promise_type = detail::awaitable_frame<T, Executor>;

    constexpr awaitable() noexcept = default;
    awaitable(awaitable&& other) noexcept : frame_{other.frame_} { other.frame_ = nullptr; }
    awaitable& operator=(awaitable&& other) noexcept {
        if (this != &other) {
            reset();
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }
    awaitable(awaitable const&) = delete;
    awaitable& operator=(awaitable const&) = delete;
    ~awaitable() { reset(); }

    bool valid() const noexcept { return frame_ != nullptr; }

    bool await_ready() const noexcept { return false; }

    template <class P> void await_suspend(co2::coroutine_handle<P> caller) noexcept {
        frame_->push_frame(&caller.promise());
    }

    // asio awaitable.hpp:125-129：移进临时对象，表达式结束时销毁子帧。
    T await_resume() { return awaitable(static_cast<awaitable&&>(*this)).frame_->get(); }

  private:
    template <class, class> friend class detail::awaitable_frame;
    template <class, class, class> friend class detail::entry_frame_impl;

    explicit awaitable(detail::awaitable_frame<T, Executor>* frame) noexcept : frame_{frame} {}

    void reset() noexcept {
        if (frame_ != nullptr) {
            frame_->destroy();
            frame_ = nullptr;
        }
    }

    detail::awaitable_frame<T, Executor>* frame_{nullptr};
};

namespace detail {

template <class T, class Executor>
awaitable<T, Executor> awaitable_frame<T, Executor>::get_return_object() noexcept {
    this->coro_ = co2::coroutine_handle<awaitable_frame>::from_promise(*this);
    return awaitable<T, Executor>{this};
}

template <class Executor>
awaitable<void, Executor> awaitable_frame<void, Executor>::get_return_object() noexcept {
    this->coro_ = co2::coroutine_handle<awaitable_frame>::from_promise(*this);
    return awaitable<void, Executor>{this};
}

} // namespace detail

// §16 D4：Executor 没有默认值，提供常用别名。
template <class T = void> using pool_awaitable = awaitable<T, thread_pool::executor_type>;
template <class T = void> using manual_awaitable = awaitable<T, manual_executor::executor_type>;

} // namespace aw

#include "awaitable/impl/awaitable_thread.hpp"
