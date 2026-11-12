#pragma once

#include <new>
#include <type_traits>

#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"

namespace aw {

// CO2_AWAIT_SET(token, aw::get_stop_token())：awaitable 帧的 await_transform 处理它（Task 13）。
struct get_stop_token_t {
    using aw_this_coro_tag = void;
};
inline get_stop_token_t get_stop_token() noexcept { return get_stop_token_t{}; }
// coro env.hpp 的旧名，逐字兼容。
inline get_stop_token_t getStopToken() noexcept { return get_stop_token_t{}; }

namespace detail {

// 移植自 coro env.hpp:25-85。
template <class Promise>
auto stop_token_of_promise(Promise& promise, int) noexcept
    -> decltype(static_cast<stop_token>(promise.get_stop_token())) {
    return promise.get_stop_token();
}
template <class Promise> stop_token stop_token_of_promise(Promise&, long) noexcept {
    return stop_token{};
}

template <class Promise>
auto inherit_from_promise(EnvStopToken& into, Promise& awaiting, int) noexcept
    -> decltype(static_cast<EnvStopToken const&>(awaiting.envStopToken()), void()) {
    into.borrow(awaiting.envStopToken());
}
template <class Promise>
void inherit_from_promise(EnvStopToken& into, Promise& awaiting, long) noexcept {
    into.own(stop_token_of_promise(awaiting, 0));
}

template <class Promise>
void inherit_from_handle(EnvStopToken& into, co2::coroutine_handle<Promise> awaiting,
                         std::false_type) noexcept {
    inherit_from_promise(into, awaiting.promise(), 0);
}
template <class Promise>
void inherit_from_handle(EnvStopToken& into, co2::coroutine_handle<Promise>,
                         std::true_type) noexcept {
    into.reset();
}

// §6.4 单向桥：cancellation_state 收到通过 in 过滤器的取消时 request_stop()。
// 桥本身来自 cancellation_signal 标签；stop_source 的停止状态按移植原样用 new 分配
// （只在有人取 token 时发生一次，不在热路径上）。
class stop_bridge {
  public:
    static stop_bridge* create() {
        void* const memory = recycling_allocate(mem_tag::cancellation_signal, sizeof(stop_bridge),
                                                alignof(stop_bridge));
        try {
            return ::new (memory) stop_bridge;
        } catch (...) {
            recycling_deallocate(mem_tag::cancellation_signal, memory, sizeof(stop_bridge));
            throw;
        }
    }

    static void destroy(stop_bridge* bridge) noexcept {
        bridge->~stop_bridge();
        recycling_deallocate(mem_tag::cancellation_signal, bridge, sizeof(stop_bridge));
    }

    void attach(cancellation_state& state) noexcept {
        state.set_observer(&on_cancel, this);
        if (!!state.cancelled()) source_.request_stop();
    }

    stop_token token() const noexcept { return source_.get_token(); }
    EnvStopToken const& env() const noexcept { return env_; }

  private:
    stop_bridge() { env_.own(source_.get_token()); }
    stop_bridge(stop_bridge const&) = delete;
    stop_bridge& operator=(stop_bridge const&) = delete;
    ~stop_bridge() = default;

    static void on_cancel(void* self, cancellation_type) noexcept {
        static_cast<stop_bridge*>(self)->source_.request_stop();
    }

    stop_source source_;
    EnvStopToken env_;
};

} // namespace detail

template <class Promise>
void inherit_stop_token(detail::EnvStopToken& into, co2::coroutine_handle<Promise> awaiting) noexcept {
    detail::inherit_from_handle(into, awaiting, std::is_void<Promise>{});
}

template <class Promise>
void inheritStopToken(detail::EnvStopToken& into, co2::coroutine_handle<Promise> awaiting) noexcept {
    inherit_stop_token(into, awaiting);
}

} // namespace aw
