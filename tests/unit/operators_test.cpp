#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "awaitable/operators.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "awaitable/variant.hpp"
#include "check.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using namespace aw::awaitable_operators;
using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

auto value_after_post(ex_t ex, int v) CO2_BEG(task<int>, (ex, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_value(ex, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

auto text(ex_t ex, std::string s) CO2_BEG(task<std::string>, (ex, s)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_RETURN(s);
}
CO2_END

auto nothing(ex_t ex) CO2_BEG(task<void>, (ex)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_RETURN();
}
CO2_END

auto fail_after_post(ex_t ex, char const* what) CO2_BEG(task<int>, (ex, what)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    throw std::runtime_error{what};
}
CO2_END

// 等事件；被取消时把 operation_aborted 作为异常抛出。
auto wait_event(event_t* ev, int v) CO2_BEG(task<int>, (ev, v)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

// 不可默认构造的值类型：co_spawn 交付 optional<T>，运算符要剥掉一层。
struct no_default {
    explicit no_default(int x) : v{x} {}
    int v;
};
auto make_no_default(ex_t ex, int v) CO2_BEG(task<no_default>, (ex, v)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_RETURN(no_default{v});
}
CO2_END

template <class T> T run(aw::manual_executor& ctx, task<T> a) {
    auto f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

std::string message_of(std::exception_ptr e) {
    try {
        std::rethrow_exception(e);
    } catch (std::exception const& x) {
        return x.what();
    }
}

auto and_values(ex_t ex) CO2_BEG(task<int>, (ex), std::tuple<int, std::string> p{}; int n{};
                                 std::tuple<int, int, int> three{};) {
    CO2_AWAIT_SET(p, value_after_post(ex, 1) && text(ex, "two"));
    if (std::get<0>(p) != 1 || std::get<1>(p) != "two") CO2_RETURN(-1);
    CO2_AWAIT_SET(n, nothing(ex) && value_after_post(ex, 5));
    if (n != 5) CO2_RETURN(-2);
    CO2_AWAIT_SET(n, value_after_post(ex, 6) && nothing(ex));
    if (n != 6) CO2_RETURN(-3);
    CO2_AWAIT(nothing(ex) && nothing(ex));
    CO2_AWAIT_SET(three, value_after_post(ex, 1) && value_after_post(ex, 2) && value_after_post(ex, 3));
    CO2_RETURN(std::get<0>(three) + std::get<1>(three) + std::get<2>(three));
}
CO2_END

void and_combines_values() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, and_values(ctx.get_executor())) == 6);
}

void and_failure_cancels_the_other() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    auto f = aw::co_spawn(ctx, wait_event(&ev, 1) && fail_after_post(ctx.get_executor(), "boom"), aw::use_future);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::terminal);
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (aw::multiple_exceptions const& m) {
        // 被取消的一边以 operation_aborted 失败，两边都失败：带第一个操作数的异常。
        AW_CHECK(message_of(m.first_exception()).find("aborted") != std::string::npos ||
                 message_of(m.first_exception()).find("canceled") != std::string::npos ||
                 message_of(m.first_exception()).find("cancelled") != std::string::npos);
    }
}

void and_single_failure_rethrows_it() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "left") && value_after_post(ctx.get_executor(), 2),
                          aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (std::runtime_error const& e) {
        AW_CHECK(std::string{e.what()} == "left");
    }
}

void and_both_fail_multiple_exceptions() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "a") && fail_after_post(ctx.get_executor(), "b"),
                          aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (aw::multiple_exceptions const& m) {
        AW_CHECK(message_of(m.first_exception()) == "a");
    }
}

void or_winner_cancels_loser() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    auto f = aw::co_spawn(ctx, wait_event(&ev, 1) || value_after_post(ctx.get_executor(), 2), aw::use_future);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::terminal);
    auto r = f.get();
    AW_CHECK(r.index() == 1U && aw::get<1>(r) == 2);
}

void or_failure_falls_back_to_other() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "x") || wait_event(&ev, 7), aw::use_future);
    ctx.run();
    AW_CHECK(ev.waiting()); // 失败不触发取消
    ev.set();
    ctx.run();
    auto r = f.get();
    AW_CHECK(r.index() == 1U && aw::get<1>(r) == 7);
}

void or_both_fail_multiple_exceptions() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "a") || fail_after_post(ctx.get_executor(), "b"),
                          aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (aw::multiple_exceptions const& m) {
        AW_CHECK(message_of(m.first_exception()) == "a");
    }
}

auto or_shapes(ex_t ex) CO2_BEG(task<int>, (ex), aw::variant<aw::monostate, int> a{};
                                aw::variant<int, aw::monostate> b{}; aw::variant<int, std::string, int> c{};
                                aw::variant<aw::monostate, aw::monostate> d{};) {
    CO2_AWAIT_SET(a, nothing(ex) || value_after_post(ex, 3));
    if (a.index() != 0U) CO2_RETURN(-1);
    CO2_AWAIT_SET(b, value_after_post(ex, 4) || nothing(ex));
    if (b.index() != 0U || aw::get<0>(b) != 4) CO2_RETURN(-2);
    CO2_AWAIT_SET(c, fail_after_post(ex, "f") || text(ex, "t") || value_after_post(ex, 9));
    if (c.index() != 1U || aw::get<1>(c) != "t") CO2_RETURN(-3);
    CO2_AWAIT_SET(d, nothing(ex) || nothing(ex));
    CO2_RETURN(static_cast<int>(d.index()) + 10);
}
CO2_END

void or_result_shapes() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, or_shapes(ctx.get_executor())) == 10);
}

void parent_cancellation_reaches_both_children() {
    aw::manual_executor ctx;
    event_t e1{ctx.get_executor()};
    event_t e2{ctx.get_executor()};
    aw::cancellation_signal sig;
    std::exception_ptr got;
    bool called = false;
    aw::co_spawn(ctx, wait_event(&e1, 1) || wait_event(&e2, 2),
                 aw::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e, aw::variant<int, int>) {
                     called = true;
                     got = e;
                 }));
    ctx.run();
    AW_CHECK(e1.waiting() && e2.waiting());
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(e1.last_cancel() == aw::cancellation_type::terminal);
    AW_CHECK(e2.last_cancel() == aw::cancellation_type::terminal);
    AW_CHECK(called && got);
}

auto non_default(ex_t ex) CO2_BEG(task<int>, (ex), std::tuple<no_default, int> p{no_default{0}, 0};) {
    CO2_AWAIT_SET(p, make_no_default(ex, 4) && value_after_post(ex, 5));
    CO2_RETURN(std::get<0>(p).v + std::get<1>(p));
}
CO2_END

void non_default_constructible_values() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, non_default(ctx.get_executor())) == 9);
}

} // namespace

int main() {
    and_combines_values();
    and_failure_cancels_the_other();
    and_single_failure_rethrows_it();
    and_both_fail_multiple_exceptions();
    or_winner_cancels_loser();
    or_failure_falls_back_to_other();
    or_both_fail_multiple_exceptions();
    or_result_shapes();
    parent_cancellation_reaches_both_children();
    non_default_constructible_values();
    return 0;
}
