#include <exception>
#include <system_error>
#include <tuple>

#include "awaitable/as_tuple.hpp"
#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/error.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "check.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

bool is_aborted(std::exception_ptr const& e) {
    if (!e) return false;
    try {
        std::rethrow_exception(e);
    } catch (std::system_error const& error) {
        return error.code() == std::error_code{aw::error::operation_aborted};
    } catch (...) {
        return false;
    }
}

struct record {
    bool called{false};
    std::exception_ptr e{};
    int value{-1};
};

struct record_int {
    record* r;
    void operator()(std::exception_ptr e, int v) const {
        r->called = true;
        r->e = e;
        r->value = v;
    }
};

// 等待事件；被取消时异常逸出到 co_spawn 的回调。
auto wait_once(event_t* ev) CO2_BEG(task<int>, (ev)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_RETURN(1);
}
CO2_END

// 改成接收 partial，用 as_tuple 接住取消，再看 cancelled() 和 throw_if_cancelled。
auto partial_then_continue(event_t* ev, ex_t ex)
    CO2_BEG(task<int>, (ev, ex), std::tuple<std::error_code> r{}; aw::cancellation_state st{};
            bool throwing{}; int v{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_partial_cancellation{}));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    if (std::get<0>(r) != std::error_code{aw::error::operation_aborted}) CO2_RETURN(-1);
    CO2_AWAIT_SET(st, aw::this_coro::cancellation_state);
    if (st.cancelled() != aw::cancellation_type::partial) CO2_RETURN(-2);
    CO2_AWAIT_SET(throwing, aw::this_coro::throw_if_cancelled());
    if (!throwing) CO2_RETURN(-3);
    CO2_AWAIT(aw::this_coro::throw_if_cancelled(false));
    CO2_AWAIT_SET(v, aw_test::async_value(ex, 7, aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

// 已记录取消且 throw_if_cancelled 默认开：下一次等待在发起之前抛出。
auto partial_then_throw(event_t* ev, ex_t ex, int* started)
    CO2_BEG(task<int>, (ev, ex, started), std::tuple<std::error_code> r{}; int v{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_partial_cancellation{}));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    ++*started;
    CO2_AWAIT_SET(v, aw_test::async_value(ex, 7, aw::use_awaitable));
    ++*started;
    CO2_RETURN(v);
}
CO2_END

// reset_cancellation_state 清除已记录的取消。
auto reset_clears(event_t* ev, ex_t ex)
    CO2_BEG(task<int>, (ev, ex), std::tuple<std::error_code> r{}; aw::cancellation_state st{}; int v{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_total_cancellation{}));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    CO2_AWAIT(aw::this_coro::reset_cancellation_state());
    CO2_AWAIT_SET(st, aw::this_coro::cancellation_state);
    if (st.cancelled() != aw::cancellation_type::none) CO2_RETURN(-1);
    CO2_AWAIT_SET(v, aw_test::async_value(ex, 9, aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

// stop_token 适配器：取消转成 request_stop（§6.4）。
auto token_observer(event_t* ev, bool* stopped)
    CO2_BEG(task<int>, (ev, stopped), aw::stop_token tok{}; std::tuple<std::error_code> r{};) {
    CO2_AWAIT_SET(tok, aw::get_stop_token());
    if (tok.stop_requested()) CO2_RETURN(-1);
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    *stopped = tok.stop_requested();
    CO2_RETURN(std::get<0>(r) == std::error_code{aw::error::operation_aborted} ? 1 : -2);
}
CO2_END

void terminal_cancel_reaches_operation() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, wait_once(&ev), aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    AW_CHECK(ev.waiting());
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::terminal);
    AW_CHECK(r.called && is_aborted(r.e));
}

void default_filter_drops_partial() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, wait_once(&ev), aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::partial);
    ctx.run();
    AW_CHECK(ev.waiting());
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::none);
    ev.set();
    ctx.run();
    AW_CHECK(r.called && !r.e && r.value == 1);
}

void partial_with_throw_if_cancelled_off() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, partial_then_continue(&ev, ctx.get_executor()),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::partial);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::partial);
    AW_CHECK(r.called && !r.e && r.value == 7);
}

void recorded_cancel_throws_on_next_await() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    int started = 0;
    aw::co_spawn(ctx, partial_then_throw(&ev, ctx.get_executor(), &started),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::partial);
    ctx.run();
    AW_CHECK(started == 1);
    AW_CHECK(r.called && is_aborted(r.e));
}

void reset_clears_recorded_cancel() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, reset_clears(&ev, ctx.get_executor()),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::total);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::total);
    AW_CHECK(r.called && !r.e && r.value == 9);
}

void stop_token_follows_cancellation() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    bool stopped = false;
    aw::co_spawn(ctx, token_observer(&ev, &stopped),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(stopped);
    AW_CHECK(r.called && !r.e && r.value == 1);
}

// 回调先完成、之后才 emit：处理器经引用计数桥安全地变成空操作。
void emit_after_completion_is_harmless() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, wait_once(&ev), aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    ev.set();
    ctx.run();
    AW_CHECK(r.called && !r.e && r.value == 1);
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
}

} // namespace

int main() {
    terminal_cancel_reaches_operation();
    default_filter_drops_partial();
    partial_with_throw_if_cancelled_off();
    recorded_cancel_throws_on_next_await();
    reset_clears_recorded_cancel();
    stop_token_follows_cancellation();
    emit_after_completion_is_harmless();
    return 0;
}
