#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw {

// §8.4：void(Ts...) 变成 void(std::tuple<Ts...>)，不再把错误转成异常（asio as_tuple.hpp:37）。
template <class Token> struct as_tuple_t {
    Token token_;
};

template <class Token> as_tuple_t<typename std::decay<Token>::type> as_tuple(Token&& token) {
    return as_tuple_t<typename std::decay<Token>::type>{std::forward<Token>(token)};
}

namespace detail {

template <class Handler, class Signature> class as_tuple_handler;

template <class Handler, class... Ts> class as_tuple_handler<Handler, void(Ts...)> {
  public:
    template <class H> explicit as_tuple_handler(H&& h) : handler_(std::forward<H>(h)) {}

    void operator()(Ts... args) { handler_(std::tuple<Ts...>(std::move(args)...)); }

    Handler const& get() const noexcept { return handler_; }

  private:
    Handler handler_;
};

template <class Initiation, class Signature> struct as_tuple_initiation {
    Initiation init;
    template <class Handler, class... Args> void operator()(Handler&& handler, Args&&... args) {
        std::move(init)(
            as_tuple_handler<typename std::decay<Handler>::type, Signature>{std::forward<Handler>(handler)},
            std::forward<Args>(args)...);
    }
};

} // namespace detail

template <class Token, class... Ts> struct async_result<as_tuple_t<Token>, void(Ts...)> {
    template <class Initiation, class RawToken, class... Args>
    static auto initiate(Initiation&& init, RawToken&& token, Args&&... args) {
        return async_initiate<Token, void(std::tuple<Ts...>)>(
            detail::as_tuple_initiation<typename std::decay<Initiation>::type, void(Ts...)>{
                std::forward<Initiation>(init)},
            token.token_, std::forward<Args>(args)...);
    }
};

// 包装后的回调转发内层回调的关联 executor / slot。
template <class Handler, class Signature, class Default>
struct associated_executor<detail::as_tuple_handler<Handler, Signature>, Default> {
    using type = typename associated_executor<Handler, Default>::type;
    static type get(detail::as_tuple_handler<Handler, Signature> const& h, Default const& d) noexcept {
        return associated_executor<Handler, Default>::get(h.get(), d);
    }
};
template <class Handler, class Signature>
struct associated_cancellation_slot<detail::as_tuple_handler<Handler, Signature>> {
    static cancellation_slot get(detail::as_tuple_handler<Handler, Signature> const& h) noexcept {
        return get_associated_cancellation_slot(h.get());
    }
};

} // namespace aw
