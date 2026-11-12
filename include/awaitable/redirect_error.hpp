#pragma once

#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw {

// §8.4：剥掉签名里打头的 error_code / exception_ptr，写进引用（asio redirect_error.hpp:36）。
// 被引用的变量必须活过挂起点：在宏协程里写在 CO2_BEG 的局部列表里（§13 第 1 条）。
template <class Token, class Error> struct redirect_error_t {
    static_assert(std::is_same<Error, std::error_code>::value ||
                      std::is_same<Error, std::exception_ptr>::value,
                  "redirect_error: the target must be std::error_code or std::exception_ptr");
    Token token_;
    Error* target_;
};

template <class Token, class Error>
redirect_error_t<typename std::decay<Token>::type, Error> redirect_error(Token&& token, Error& target) {
    return redirect_error_t<typename std::decay<Token>::type, Error>{std::forward<Token>(token), &target};
}

namespace detail {

template <class Handler, class Error, class Signature> class redirect_error_handler;

template <class Handler, class Error, class... Ts> class redirect_error_handler<Handler, Error, void(Ts...)> {
  public:
    template <class H>
    redirect_error_handler(H&& h, Error* target) : handler_(std::forward<H>(h)), target_{target} {}

    void operator()(Error error, Ts... args) {
        *target_ = std::move(error);
        handler_(std::move(args)...);
    }

    Handler const& get() const noexcept { return handler_; }

  private:
    Handler handler_;
    Error* target_;
};

template <class Initiation, class Error, class Signature> struct redirect_error_initiation {
    Initiation init;
    Error* target;
    template <class Handler, class... Args> void operator()(Handler&& handler, Args&&... args) {
        std::move(init)(redirect_error_handler<typename std::decay<Handler>::type, Error, Signature>{
                            std::forward<Handler>(handler), target},
                        std::forward<Args>(args)...);
    }
};

} // namespace detail

template <class Token, class Error, class... Ts>
struct async_result<redirect_error_t<Token, Error>, void(Error, Ts...)> {
    template <class Initiation, class RawToken, class... Args>
    static auto initiate(Initiation&& init, RawToken&& token, Args&&... args) {
        return async_initiate<Token, void(Ts...)>(
            detail::redirect_error_initiation<typename std::decay<Initiation>::type, Error, void(Ts...)>{
                std::forward<Initiation>(init), token.target_},
            token.token_, std::forward<Args>(args)...);
    }
};

template <class Handler, class Error, class Signature, class Default>
struct associated_executor<detail::redirect_error_handler<Handler, Error, Signature>, Default> {
    using type = typename associated_executor<Handler, Default>::type;
    static type get(detail::redirect_error_handler<Handler, Error, Signature> const& h,
                    Default const& d) noexcept {
        return associated_executor<Handler, Default>::get(h.get(), d);
    }
};
template <class Handler, class Error, class Signature>
struct associated_cancellation_slot<detail::redirect_error_handler<Handler, Error, Signature>> {
    static cancellation_slot get(detail::redirect_error_handler<Handler, Error, Signature> const& h) noexcept {
        return get_associated_cancellation_slot(h.get());
    }
};

} // namespace aw
