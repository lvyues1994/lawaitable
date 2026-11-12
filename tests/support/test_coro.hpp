#pragma once

#include <exception>

#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"

namespace aw_test {

// 最小的 core 级协程：立即开始，结束时帧自毁（final_suspend 不挂起）。只用来测 schedule。
struct eager_coro {
    struct promise_type {
        eager_coro get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() const noexcept { return {}; }
        co2::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

} // namespace aw_test
