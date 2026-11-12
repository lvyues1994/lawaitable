#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw {
namespace detail {

// asio work_dispatcher：在 ex 上执行，然后派发到回调自己的关联 executor。
template <class Handler, class HandlerExecutor> struct work_dispatcher {
    Handler handler;
    HandlerExecutor handler_ex;
    void operator()() { handler_ex.dispatch(std::move(handler)); }
};

template <class Executor> struct initiate_post {
    Executor ex;
    template <class Handler> void operator()(Handler&& handler) const {
        using handler_type = typename std::decay<Handler>::type;
        auto target = get_associated_executor(handler, ex);
        ex.post(work_dispatcher<handler_type, decltype(target)>{std::forward<Handler>(handler), target});
    }
};

} // namespace detail

template <class Executor, class Token> auto post(Executor const& ex, Token&& token) {
    return async_initiate<Token, void()>(detail::initiate_post<Executor>{ex}, token);
}

} // namespace aw
