#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/impl/async_op_awaiter.hpp"

namespace aw {

// §8.3：Executor = void 时取等待它的执行线程的 executor；否则必须与之相同（编译期检查）。
template <class Executor = void> struct use_awaitable_t {
    constexpr use_awaitable_t() noexcept = default;
};
constexpr use_awaitable_t<> use_awaitable{};

template <class Executor, class... Ts> struct async_result<use_awaitable_t<Executor>, void(Ts...)> {
    template <class Initiation, class RawToken, class... Args>
    static detail::async_op_awaiter<void(Ts...), Executor, typename std::decay<Initiation>::type,
                                    typename std::decay<Args>::type...>
    initiate(Initiation&& init, RawToken&&, Args&&... args) {
        return detail::async_op_awaiter<void(Ts...), Executor, typename std::decay<Initiation>::type,
                                        typename std::decay<Args>::type...>{
            std::forward<Initiation>(init), std::forward<Args>(args)...};
    }
};

} // namespace aw
