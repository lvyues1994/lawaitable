#pragma once

#include <type_traits>
#include <utility>

namespace aw {

// §8.1 定制点：默认把 token 原样当回调交给 initiation。
template <class Token, class Signature> struct async_result {
    template <class Initiation, class RawToken, class... Args>
    static void initiate(Initiation&& init, RawToken&& token, Args&&... args) {
        std::forward<Initiation>(init)(std::forward<RawToken>(token), std::forward<Args>(args)...);
    }
};

template <class Token, class Signature, class Initiation, class... Args>
auto async_initiate(Initiation&& init, typename std::remove_reference<Token>::type& token,
                    Args&&... args) {
    return async_result<typename std::decay<Token>::type, Signature>::initiate(
        std::forward<Initiation>(init), static_cast<Token&&>(token), std::forward<Args>(args)...);
}

} // namespace aw
