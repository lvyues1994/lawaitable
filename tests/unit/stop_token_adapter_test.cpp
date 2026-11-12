#include <exception>
#include <type_traits>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"
#include "check.hpp"

namespace {

// 一个提供 envStopToken() 的 core 级 promise：inherit_stop_token 应当借用它的状态。
struct env_coro {
    struct promise_type {
        aw::detail::EnvStopToken env;
        aw::detail::EnvStopToken const& envStopToken() const noexcept { return env; }
        env_coro get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() const noexcept { return {}; }
        co2::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

// 只提供 get_stop_token() 的 promise：inherit_stop_token 应当自己持有一份引用。
struct token_coro {
    struct promise_type {
        aw::stop_token token;
        aw::stop_token get_stop_token() const noexcept { return token; }
        token_coro get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() const noexcept { return {}; }
        co2::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

struct capture_env {
    aw::detail::EnvStopToken* into;
    aw::stop_source* source;
    bool await_ready() const noexcept { return false; }
    template <class P> bool await_suspend(co2::coroutine_handle<P> h) noexcept {
        h.promise().set_source(*source);
        aw::inherit_stop_token(*into, h);
        return false;
    }
    void await_resume() const noexcept {}
};

struct env_coro_with_source {
    struct promise_type : env_coro::promise_type {
        void set_source(aw::stop_source& s) noexcept { env.own(s.get_token()); }
        env_coro_with_source get_return_object() noexcept { return {}; }
    };
};

struct token_coro_with_source {
    struct promise_type : token_coro::promise_type {
        void set_source(aw::stop_source& s) noexcept { token = s.get_token(); }
        token_coro_with_source get_return_object() noexcept { return {}; }
    };
};

auto borrowFromEnv(aw::detail::EnvStopToken& into, aw::stop_source& source, bool& requested)
    CO2_BEG(env_coro_with_source, (into, source, requested)) {
    CO2_AWAIT((capture_env{&into, &source}));
    source.request_stop();
    requested = into.get().stop_requested();
    into.reset(); // 借用的状态只在等待者活着时有效
    CO2_RETURN();
}
CO2_END

auto ownFromToken(aw::detail::EnvStopToken& into, aw::stop_source& source)
    CO2_BEG(token_coro_with_source, (into, source)) {
    CO2_AWAIT((capture_env{&into, &source}));
    CO2_RETURN();
}
CO2_END

void inheritBorrowsOrOwns() {
    aw::stop_source source;
    aw::detail::EnvStopToken borrowed;
    bool requested = false;
    borrowFromEnv(borrowed, source, requested);
    AW_CHECK(requested);

    aw::stop_source other;
    aw::detail::EnvStopToken owned;
    ownFromToken(owned, other); // 协程已结束：owned 必须自己持有引用
    other.request_stop();
    AW_CHECK(owned.get().stop_requested());
}

void bridgeTurnsCancellationIntoStopRequest() {
    aw::detail::thread_context context;
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()};
    aw::detail::stop_bridge* bridge = aw::detail::stop_bridge::create();
    bridge->attach(state);
    aw::stop_token token = bridge->token();
    bool called = false;
    aw::stop_callback<> callback{token, [&] { called = true; }};
    AW_CHECK(token.stop_possible() && !token.stop_requested());
    parent.emit(aw::cancellation_type::partial); // 默认过滤器拦掉
    AW_CHECK(!token.stop_requested());
    parent.emit(aw::cancellation_type::terminal);
    AW_CHECK(token.stop_requested() && called);
    AW_CHECK(bridge->env().get().stop_requested());
    aw::detail::stop_bridge::destroy(bridge);
}

void attachAfterCancellationRequestsImmediately() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()};
    parent.emit(aw::cancellation_type::terminal);
    aw::detail::stop_bridge* bridge = aw::detail::stop_bridge::create();
    bridge->attach(state);
    AW_CHECK(bridge->token().stop_requested());
    aw::detail::stop_bridge::destroy(bridge);
}

void legacyNames() {
    static_assert(std::is_same<decltype(aw::getStopToken()), aw::get_stop_token_t>::value, "");
    static_assert(std::is_same<decltype(aw::get_stop_token()), aw::get_stop_token_t>::value, "");
}

} // namespace

int main() {
    inheritBorrowsOrOwns();
    bridgeTurnsCancellationIntoStopRequest();
    attachAfterCancellationRequestsImmediately();
    legacyNames();
    return 0;
}
