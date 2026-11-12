#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/impl/awaitable_frame.hpp"
#include "awaitable/optional.hpp"

namespace aw {

template <class T, class Executor> class async_generator;

namespace detail {

// §10.2：async_generator 的 promise。与 awaitable 共用 awaitable_frame_base（压栈 / 出栈、
// await_transform、取消、stop_token 适配器、回收缓存）。
template <class T, class Executor> class generator_frame : public awaitable_frame_base<Executor> {
    static_assert(std::is_object<T>::value && !std::is_const<T>::value,
                  "async_generator<T, E>: T must be a non-const object type");

  public:
    async_generator<T, Executor> get_return_object() noexcept;

    // yield：记下值的地址，然后出栈回到消费者（§10.2 第 2 条）。
    struct yield_awaiter {
        generator_frame* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept {
            self->active_ = false;
            self->pop_frame();
        }
        void await_resume() const noexcept {}
    };

    // 左值：只取地址，消费者拷贝构造；右值：移进帧内存储，消费者再移出。
    yield_awaiter yield_value(T const& value) noexcept {
        yielded_ = &value;
        stored_.reset();
        return yield_awaiter{this};
    }
    yield_awaiter yield_value(T&& value) {
        stored_.emplace(std::move(value));
        yielded_ = nullptr;
        return yield_awaiter{this};
    }

    void return_void() noexcept {}

    struct final_awaiter {
        generator_frame* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept {
            self->finished_ = true;
            self->active_ = false;
            self->pop_frame();
        }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() noexcept { return final_awaiter{this}; }

    // 消费者的 await_resume 调用：取出本次的值，或空（结束），或重抛生成器体的异常。
    aw::optional<T> take() {
        if (finished_) {
            this->rethrow_exception();
            return aw::optional<T>{};
        }
        if (yielded_ != nullptr) return aw::optional<T>{*yielded_};
        aw::optional<T> out{std::move(*stored_)};
        stored_.reset();
        return out;
    }

    bool active_{false};
    bool finished_{false};

  private:
    T const* yielded_{nullptr};
    aw::optional<T> stored_{};
};

} // namespace detail
} // namespace aw
