#pragma once

#include <exception>
#include <future>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"

namespace aw {

struct use_future_t {
    constexpr use_future_t() noexcept = default;
};
constexpr use_future_t use_future{};

namespace detail {

template <class... Ts> struct future_value { using type = std::tuple<typename std::decay<Ts>::type...>; };
template <> struct future_value<> { using type = void; };
template <class T> struct future_value<T> { using type = typename std::decay<T>::type; };

// lead：0 = 没有错误参数，1 = error_code，2 = exception_ptr。
template <class Signature> struct future_signature;
template <class... Ts> struct future_signature<void(Ts...)> {
    using value_type = typename future_value<Ts...>::type;
    static constexpr int lead = 0;
};
template <class... Ts> struct future_signature<void(std::error_code, Ts...)> {
    using value_type = typename future_value<Ts...>::type;
    static constexpr int lead = 1;
};
template <class... Ts> struct future_signature<void(std::exception_ptr, Ts...)> {
    using value_type = typename future_value<Ts...>::type;
    static constexpr int lead = 2;
};

template <class V> struct promise_setter {
    template <class T> static void set(std::promise<V>& p, T&& v) { p.set_value(std::forward<T>(v)); }
    template <class... Ts, typename std::enable_if<(sizeof...(Ts) > 1U), int>::type = 0>
    static void set(std::promise<V>& p, Ts&&... vs) {
        p.set_value(V(std::forward<Ts>(vs)...));
    }
};
template <> struct promise_setter<void> {
    static void set(std::promise<void>& p) { p.set_value(); }
};

template <class Signature> class use_future_handler {
    using traits = future_signature<Signature>;
    using value_type = typename traits::value_type;

  public:
    use_future_handler() = default;
    std::future<value_type> get_future() { return promise_.get_future(); }

    template <class... Args> void operator()(Args&&... args) {
        deliver(std::integral_constant<int, traits::lead>{}, std::forward<Args>(args)...);
    }

  private:
    template <class... Args> void deliver(std::integral_constant<int, 0>, Args&&... args) {
        promise_setter<value_type>::set(promise_, std::forward<Args>(args)...);
    }
    template <class... Args>
    void deliver(std::integral_constant<int, 1>, std::error_code const& ec, Args&&... args) {
        if (ec) {
            promise_.set_exception(std::make_exception_ptr(std::system_error{ec}));
            return;
        }
        promise_setter<value_type>::set(promise_, std::forward<Args>(args)...);
    }
    template <class... Args>
    void deliver(std::integral_constant<int, 2>, std::exception_ptr const& e, Args&&... args) {
        if (e) {
            promise_.set_exception(e);
            return;
        }
        promise_setter<value_type>::set(promise_, std::forward<Args>(args)...);
    }

    std::promise<value_type> promise_;
};

} // namespace detail

template <class Signature> struct async_result<use_future_t, Signature> {
    template <class Initiation, class RawToken, class... Args>
    static auto initiate(Initiation&& init, RawToken&&, Args&&... args) {
        detail::use_future_handler<Signature> handler;
        auto future = handler.get_future();
        std::forward<Initiation>(init)(std::move(handler), std::forward<Args>(args)...);
        return future;
    }
};

} // namespace aw
