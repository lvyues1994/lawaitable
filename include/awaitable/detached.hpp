#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"

namespace aw {

struct detached_t {
    constexpr detached_t() noexcept = default;
};
constexpr detached_t detached{};

namespace detail {
// §7.2 第 3 条：非空 exception_ptr 在交付点重抛（相对 asio 的偏离）；其余参数忽略。
struct detached_handler {
    void operator()() const noexcept {}
    template <class... Rest> void operator()(std::exception_ptr const& e, Rest&&...) const {
        if (e) std::rethrow_exception(e);
    }
    template <class First, class... Rest,
              typename std::enable_if<!std::is_same<typename std::decay<First>::type,
                                                    std::exception_ptr>::value,
                                      int>::type = 0>
    void operator()(First&&, Rest&&...) const noexcept {}
};
} // namespace detail

template <class Signature> struct async_result<detached_t, Signature> {
    template <class Initiation, class RawToken, class... Args>
    static void initiate(Initiation&& init, RawToken&&, Args&&... args) {
        std::forward<Initiation>(init)(detail::detached_handler{}, std::forward<Args>(args)...);
    }
};

} // namespace aw
