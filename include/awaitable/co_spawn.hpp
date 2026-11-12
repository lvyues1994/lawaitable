#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/exec/executor.hpp"
#include "awaitable/impl/entry_point.hpp"

namespace aw {

// §8.2：只接受 awaitable 对象；完成签名 void(exception_ptr, T) 或 void(exception_ptr)。
template <class Executor, class T, class Token,
          typename std::enable_if<is_executor<Executor>::value, int>::type = 0>
auto co_spawn(Executor const& ex, awaitable<T, Executor> a, Token&& token) {
    static_assert(!std::is_reference<T>::value,
                  "co_spawn: the awaitable must return void or an object type");
    return async_initiate<Token, typename detail::spawn_signature<T>::type>(
        detail::initiate_co_spawn<Executor>{ex}, token, std::move(a));
}

template <class Context, class T, class Token,
          typename std::enable_if<!is_executor<Context>::value, int>::type = 0>
auto co_spawn(Context& ctx, awaitable<T, typename Context::executor_type> a, Token&& token) {
    return co_spawn(ctx.get_executor(), std::move(a), std::forward<Token>(token));
}

} // namespace aw
