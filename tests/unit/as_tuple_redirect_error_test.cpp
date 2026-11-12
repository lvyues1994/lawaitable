#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>

#include "awaitable/as_tuple.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/redirect_error.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "test_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;

auto tuple_of_error(ex_t ex) CO2_BEG(task<bool>, (ex), std::tuple<std::error_code, int> r{};) {
    CO2_AWAIT_SET(r, aw_test::async_error(ex, std::make_error_code(std::errc::timed_out),
                                          aw::as_tuple(aw::use_awaitable)));
    CO2_RETURN(std::get<0>(r) == std::make_error_code(std::errc::timed_out) && std::get<1>(r) == 0);
}
CO2_END

auto redirected(ex_t ex) CO2_BEG(task<bool>, (ex), std::error_code ec{}; int a{}; int b{};) {
    CO2_AWAIT_SET(a, aw_test::async_error(ex, std::make_error_code(std::errc::timed_out),
                                          aw::redirect_error(aw::use_awaitable, ec)));
    if (ec != std::make_error_code(std::errc::timed_out) || a != 0) CO2_RETURN(false);
    CO2_AWAIT_SET(b, aw_test::async_value(ex, 9, aw::redirect_error(aw::use_awaitable, ec)));
    CO2_RETURN(!ec && b == 9);
}
CO2_END

auto child(int v) CO2_BEG(task<int>, (v)) {
    if (v < 0) throw std::runtime_error{"negative"};
    CO2_RETURN(v);
}
CO2_END

// §7.4：拿到子协程的异常而不传播。
auto catches_child(ex_t ex)
    CO2_BEG(task<std::string>, (ex), std::tuple<std::exception_ptr, int> r{}; std::exception_ptr e{};
            int v{};) {
    CO2_AWAIT_SET(r, aw::co_spawn(ex, child(-1), aw::as_tuple(aw::use_awaitable)));
    CO2_AWAIT_SET(v, aw::co_spawn(ex, child(-2), aw::redirect_error(aw::use_awaitable, e)));
    std::string out;
    try {
        std::rethrow_exception(std::get<0>(r));
    } catch (std::runtime_error const& error) {
        out += error.what();
    }
    try {
        std::rethrow_exception(e);
    } catch (std::runtime_error const& error) {
        out += error.what();
    }
    CO2_RETURN(out + std::to_string(v));
}
CO2_END

template <class T, class Awaitable> T run_to_value(aw::manual_executor& ctx, Awaitable a) {
    std::future<T> f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

void asTupleKeepsTheErrorCode() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<bool>(ctx, tuple_of_error(ctx.get_executor())));
}

void redirectErrorWritesTheTarget() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<bool>(ctx, redirected(ctx.get_executor())));
}

void childExceptionsCanBeCaptured() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<std::string>(ctx, catches_child(ctx.get_executor())) == "negativenegative0");
}

void asTupleWithCallbackAndFuture() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::tuple<std::error_code, int> got{};
    aw_test::async_value(ex, 3, aw::as_tuple([&](std::tuple<std::error_code, int> t) { got = t; }));
    std::future<std::tuple<std::error_code, int>> f = aw_test::async_value(ex, 4, aw::as_tuple(aw::use_future));
    ctx.run();
    AW_CHECK(!std::get<0>(got) && std::get<1>(got) == 3);
    AW_CHECK(std::get<1>(f.get()) == 4);
}

void wrappersForwardAssociations() {
    aw::manual_executor io;
    aw::manual_executor target;
    bool on_target = false;
    auto bound = aw::bind_executor(target.get_executor(), [&](std::tuple<std::error_code, int>) {
        on_target = target.get_executor().running_in_this_thread();
    });
    aw_test::async_value(io.get_executor(), 1, aw::as_tuple(bound));
    io.run();
    AW_CHECK(!on_target && target.pending() == 1U);
    target.run();
    AW_CHECK(on_target);

    aw::cancellation_signal signal;
    auto slot_bound = aw::bind_cancellation_slot(signal.slot(), [](int) {});
    using inner = decltype(slot_bound);
    aw::detail::redirect_error_handler<inner, std::error_code, void(int)> redirect{slot_bound, nullptr};
    aw::detail::as_tuple_handler<decltype(redirect), void(std::error_code, int)> wrapped{redirect};
    AW_CHECK(aw::get_associated_cancellation_slot(wrapped) == signal.slot());
}

} // namespace

int main() {
    asTupleKeepsTheErrorCode();
    redirectErrorWritesTheTarget();
    childExceptionsCanBeCaptured();
    asTupleWithCallbackAndFuture();
    wrappersForwardAssociations();
    return 0;
}
