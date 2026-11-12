#pragma once

#include "awaitable/cancel/cancellation_state.hpp"

namespace aw {
namespace this_coro {

// CO2_AWAIT_SET(ex, aw::this_coro::executor);
struct executor_t {
    using aw_this_coro_tag = void;
    constexpr executor_t() noexcept = default;
};
constexpr executor_t executor{};

// §4.5 / §6.2：CO2_AWAIT_SET(state, aw::this_coro::cancellation_state);
struct cancellation_state_t {
    using aw_this_coro_tag = void;
    constexpr cancellation_state_t() noexcept = default;
};
constexpr cancellation_state_t cancellation_state{};

// CO2_AWAIT(aw::this_coro::reset_cancellation_state(...))：用新过滤器重建取消状态并清除已记录的取消
// （asio this_coro.hpp:76-140）。
template <class In, class Out> struct reset_cancellation_state_t {
    using aw_this_coro_tag = void;
    In in;
    Out out;
};

inline reset_cancellation_state_t<enable_terminal_cancellation, enable_terminal_cancellation>
reset_cancellation_state() noexcept {
    return {enable_terminal_cancellation{}, enable_terminal_cancellation{}};
}
template <class Filter>
reset_cancellation_state_t<Filter, Filter> reset_cancellation_state(Filter filter) {
    return {filter, filter};
}
template <class In, class Out>
reset_cancellation_state_t<In, Out> reset_cancellation_state(In in, Out out) {
    return {in, out};
}

// CO2_AWAIT_SET(on, aw::this_coro::throw_if_cancelled()); CO2_AWAIT(aw::this_coro::throw_if_cancelled(false));
struct throw_if_cancelled_0_t {
    using aw_this_coro_tag = void;
};
struct throw_if_cancelled_1_t {
    using aw_this_coro_tag = void;
    bool value;
};
inline throw_if_cancelled_0_t throw_if_cancelled() noexcept { return {}; }
inline throw_if_cancelled_1_t throw_if_cancelled(bool value) noexcept { return {value}; }

} // namespace this_coro
} // namespace aw
