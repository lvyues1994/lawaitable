#pragma once

namespace aw {
namespace this_coro {

// CO2_AWAIT_SET(ex, aw::this_coro::executor);
struct executor_t {
    using aw_this_coro_tag = void;
    constexpr executor_t() noexcept = default;
};
constexpr executor_t executor{};

} // namespace this_coro
} // namespace aw
