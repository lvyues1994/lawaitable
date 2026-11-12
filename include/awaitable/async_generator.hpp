#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/awaitable.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"
#include "awaitable/impl/generator_frame.hpp"
#include "awaitable/optional.hpp"

namespace aw {

// §10.1：CO2_AWAIT_SET(item, gen.next()); item 是 aw::optional<T>，空表示结束。
// 生成器在消费者的执行线程上运行：next() 把生成器帧压到消费者之上，yield 时出栈。
template <class T, class Executor> class async_generator {
  public:
    using value_type = T;
    using executor_type = Executor;
    using promise_type = detail::generator_frame<T, Executor>;

    async_generator() noexcept = default;
    async_generator(async_generator&& other) noexcept : frame_{other.frame_} { other.frame_ = nullptr; }
    async_generator& operator=(async_generator&& other) noexcept {
        if (this != &other) {
            reset();
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }
    async_generator(async_generator const&) = delete;
    async_generator& operator=(async_generator const&) = delete;
    ~async_generator() { reset(); }

    class next_awaiter {
      public:
        using aw_awaitable_tag = void; // 经 await_transform：取消检查同 awaitable（§4.3 第 4 条）

        explicit next_awaiter(promise_type* frame) noexcept : frame_{frame} {}

        bool await_ready() const noexcept { return frame_ == nullptr || frame_->finished_; }

        template <class P> void await_suspend(co2::coroutine_handle<P> consumer) noexcept {
            static_assert(std::is_same<typename P::executor_type, Executor>::value,
                          "async_generator<T, E>::next() must be awaited in a frame with executor E");
            // §10.2 第 7 条：同一时刻最多一个未完成的 next()。
            CO2_CONTRACT_CHECK(!frame_->active_);
            frame_->active_ = true;
            frame_->push_frame(&consumer.promise());
        }

        aw::optional<T> await_resume() {
            if (frame_ == nullptr) return aw::optional<T>{};
            return frame_->take();
        }

      private:
        promise_type* frame_;
    };

    next_awaiter next() noexcept { return next_awaiter{frame_}; }

  private:
    friend class detail::generator_frame<T, Executor>;
    explicit async_generator(promise_type* frame) noexcept : frame_{frame} {}

    void reset() noexcept {
        if (frame_ == nullptr) return;
        // §10.2 第 6 条：生成器正在执行或挂起在异步操作上（在帧栈里）时销毁是契约违规。
        CO2_CONTRACT_CHECK(!frame_->active_);
        frame_->destroy();
        frame_ = nullptr;
    }

    promise_type* frame_{nullptr};
};

namespace detail {
template <class T, class Executor>
async_generator<T, Executor> generator_frame<T, Executor>::get_return_object() noexcept {
    this->coro_ = co2::coroutine_handle<generator_frame>::from_promise(*this);
    return async_generator<T, Executor>{this};
}
} // namespace detail

} // namespace aw
