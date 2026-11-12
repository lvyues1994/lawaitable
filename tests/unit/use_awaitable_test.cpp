#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;

auto add_two_values(ex_t ex) CO2_BEG(task<int>, (ex), int a{}; int b{};) {
    CO2_AWAIT_SET(a, aw_test::async_value(ex, 40, aw::use_awaitable));
    CO2_AWAIT_SET(b, aw_test::async_value(ex, 2, aw::use_awaitable));
    CO2_RETURN(a + b);
}
CO2_END

auto await_error(ex_t ex) CO2_BEG(task<int>, (ex), int a{};) {
    CO2_AWAIT_SET(a, aw_test::async_error(ex, std::make_error_code(std::errc::timed_out),
                                          aw::use_awaitable));
    CO2_RETURN(a);
}
CO2_END

auto await_exception(ex_t ex) CO2_BEG(task<void>, (ex)) {
    CO2_AWAIT(aw_test::async_exception(ex, std::make_exception_ptr(std::logic_error{"op"}),
                                       aw::use_awaitable));
    CO2_RETURN();
}
CO2_END

auto void_and_tuple(ex_t ex) CO2_BEG(task<std::string>, (ex), std::tuple<int, std::string> p{};) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_AWAIT(aw_test::async_exception(ex, std::exception_ptr{}, aw::use_awaitable));
    CO2_AWAIT_SET(p, aw_test::async_pair(ex, 7, std::string{"seven"}, aw::use_awaitable));
    CO2_RETURN(std::to_string(std::get<0>(p)) + std::get<1>(p));
}
CO2_END

auto immediate(std::vector<std::string>& trace) CO2_BEG(task<int>, (trace), int v{};) {
    trace.push_back("before");
    CO2_AWAIT_SET(v, aw_test::async_immediate(trace, 5, aw::use_awaitable));
    trace.push_back("after");
    CO2_RETURN(v);
}
CO2_END

auto many_awaits(ex_t ex, int n, long* news)
    CO2_BEG(task<int>, (ex, n, news), int i{}; int r{}; int sum{}; aw_test::allocation_counter* counter{};) {
    for (i = 0; i != n; ++i) {
        if (i == 100) counter = new aw_test::allocation_counter{};
        CO2_AWAIT_SET(r, aw_test::async_value(ex, i, aw::use_awaitable));
        sum += r;
    }
    *news = counter->news();
    delete counter;
    CO2_RETURN(sum);
}
CO2_END

using pool_ex_t = aw::thread_pool::executor_type;

auto resumes_on_pool(aw_test::foreign_threads& threads)
    CO2_BEG(aw::pool_awaitable<bool>, (threads), pool_ex_t ex{}; int v{};) {
    CO2_AWAIT_SET(ex, aw::this_coro::executor);
    CO2_AWAIT_SET(v, aw_test::async_from_foreign_thread(threads, 3, aw::use_awaitable));
    CO2_RETURN(v == 3 && ex.running_in_this_thread());
}
CO2_END

auto child(int v) CO2_BEG(task<int>, (v)) {
    if (v < 0) throw std::runtime_error{"negative"};
    CO2_RETURN(v * 2);
}
CO2_END

auto pool_child(int v) CO2_BEG(aw::pool_awaitable<int>, (v)) { CO2_RETURN(v * 3); }
CO2_END

auto spawn_and_wait(ex_t ex, aw::thread_pool* pool)
    CO2_BEG(task<bool>, (ex, pool), int a{}; int b{};) {
    CO2_AWAIT_SET(a, aw::co_spawn(ex, child(5), aw::use_awaitable));
    CO2_AWAIT_SET(b, aw::co_spawn(pool->get_executor(), pool_child(5), aw::use_awaitable));
    CO2_RETURN(a == 10 && b == 15 && ex.running_in_this_thread());
}
CO2_END

auto spawn_failing(ex_t ex) CO2_BEG(task<int>, (ex), int a{};) {
    CO2_AWAIT_SET(a, aw::co_spawn(ex, child(-1), aw::use_awaitable));
    CO2_RETURN(a);
}
CO2_END

template <class T, class Awaitable> T run_to_value(aw::manual_executor& ctx, Awaitable a) {
    std::future<T> f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

void valuesAreDelivered() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<int>(ctx, add_two_values(ctx.get_executor())) == 42);
}

void errorCodesBecomeSystemErrors() {
    aw::manual_executor ctx;
    std::future<int> f = aw::co_spawn(ctx, await_error(ctx.get_executor()), aw::use_future);
    ctx.run();
    bool caught = false;
    try {
        static_cast<void>(f.get());
    } catch (std::system_error const& e) {
        caught = e.code() == std::make_error_code(std::errc::timed_out);
    }
    AW_CHECK(caught);
}

void exceptionPointersAreRethrown() {
    aw::manual_executor ctx;
    std::future<void> f = aw::co_spawn(ctx, await_exception(ctx.get_executor()), aw::use_future);
    ctx.run();
    bool caught = false;
    try {
        f.get();
    } catch (std::logic_error const& e) {
        caught = std::string{e.what()} == "op";
    }
    AW_CHECK(caught);
}

void voidAndTupleResults() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<std::string>(ctx, void_and_tuple(ctx.get_executor())) == "7seven");
}

void synchronousCompletionResumesViaPost() {
    aw::manual_executor ctx;
    std::vector<std::string> trace;
    AW_CHECK(run_to_value<int>(ctx, immediate(trace)) == 5);
    AW_CHECK((trace == std::vector<std::string>{"before", "init-begin", "init-end", "after"}));
}

void awaiterFitsTheSlot() {
    aw::manual_executor ctx;
    using awaiter = decltype(aw_test::async_value(ctx.get_executor(), 1, aw::use_awaitable));
    static_assert(sizeof(awaiter) <= 64U, "the standard test operation's awaiter must fit the 64 B slot");
    AW_CHECK(sizeof(awaiter) <= 64U);
}

void sequentialAwaitsDoNotAllocate() {
    aw::manual_executor ctx;
    long news = -1;
    AW_CHECK(run_to_value<int>(ctx, many_awaits(ctx.get_executor(), 1100, &news)) == 1099 * 1100 / 2);
    if (aw_test::counting_new_enabled) AW_CHECK(news == 0);
}

void foreignCompletionResumesOnTheExecutor() {
    aw_test::foreign_threads threads;
    aw::thread_pool pool{2};
    std::future<bool> f = aw::co_spawn(pool, resumes_on_pool(threads), aw::use_future);
    AW_CHECK(f.get());
}

void coSpawnCanBeAwaited() {
    aw::thread_pool pool{1};
    aw::manual_executor ctx;
    std::future<bool> f = aw::co_spawn(ctx, spawn_and_wait(ctx.get_executor(), &pool), aw::use_future);
    while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) ctx.run_one();
    AW_CHECK(f.get());
}

void awaitedCoSpawnRethrows() {
    aw::manual_executor ctx;
    std::future<int> f = aw::co_spawn(ctx, spawn_failing(ctx.get_executor()), aw::use_future);
    ctx.run();
    bool caught = false;
    try {
        static_cast<void>(f.get());
    } catch (std::runtime_error const& e) {
        caught = std::string{e.what()} == "negative";
    }
    AW_CHECK(caught);
}

} // namespace

int main() {
    valuesAreDelivered();
    errorCodesBecomeSystemErrors();
    exceptionPointersAreRethrown();
    voidAndTupleResults();
    synchronousCompletionResumesViaPost();
    awaiterFitsTheSlot();
    sequentialAwaitsDoNotAllocate();
    foreignCompletionResumesOnTheExecutor();
    coSpawnCanBeAwaited();
    awaitedCoSpawnRethrows();
    return 0;
}
