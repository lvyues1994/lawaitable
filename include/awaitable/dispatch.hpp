#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/post.hpp"

namespace aw {
namespace detail {

template <class Executor> struct initiate_dispatch {
    Executor ex;
    template <class Handler> void operator()(Handler&& handler) const {
        using handler_type = typename std::decay<Handler>::type;
        auto target = get_associated_executor(handler, ex);
        ex.dispatch(work_dispatcher<handler_type, decltype(target)>{std::forward<Handler>(handler), target});
    }
};

} // namespace detail

template <class Executor, class Token> auto dispatch(Executor const& ex, Token&& token) {
    return async_initiate<Token, void()>(detail::initiate_dispatch<Executor>{ex}, token);
}

} // namespace aw
