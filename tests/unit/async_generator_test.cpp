#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "awaitable/async_generator.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
template <class T> using gen = aw::async_generator<T, ex_t>;

auto numbers(int n) CO2_BEG((gen<int>), (n), int i{};) {
    for (i = 0; i != n; ++i) CO2_YIELD(i);
    CO2_RETURN();
}
CO2_END

// 生成器体内等待异步操作：走与 awaitable 相同的桥接。
auto async_numbers(ex_t ex, int n) CO2_BEG((gen<int>), (ex, n), int i{}; int v{};) {
    for (i = 0; i != n; ++i) {
        CO2_AWAIT_SET(v, aw_test::async_value(ex, i * 10, aw::use_awaitable));
        CO2_YIELD(v);
    }
    CO2_RETURN();
}
CO2_END

auto words() CO2_BEG((gen<std::string>), ()) {
    CO2_YIELD(std::string{"a"});
    CO2_YIELD(std::string{"b"});
    CO2_RETURN();
}
CO2_END

auto throws_after_one() CO2_BEG((gen<int>), ()) {
    CO2_YIELD(1);
    throw std::runtime_error{"gen"};
}
CO2_END

// 生成器看到的 executor 是消费者执行线程的 executor。
auto executor_probe(ex_t expected, bool* same) CO2_BEG((gen<int>), (expected, same), ex_t ex{};) {
    CO2_AWAIT_SET(ex, aw::this_coro::executor);
    *same = ex == expected;
    CO2_YIELD(0);
    CO2_RETURN();
}
CO2_END

auto sum(gen<int> g) CO2_BEG(task<int>, (g), aw::optional<int> v{}; int total{};) {
    for (;;) {
        CO2_AWAIT_SET(v, g.next());
        if (!v) break;
        total += *v;
    }
    CO2_RETURN(total);
}
CO2_END

auto concat(gen<std::string> g) CO2_BEG(task<std::string>, (g), aw::optional<std::string> v{}; std::string s{};) {
    for (;;) {
        CO2_AWAIT_SET(v, g.next());
        if (!v) break;
        s += *v;
    }
    CO2_RETURN(s);
}
CO2_END

auto first_then_error(gen<int> g) CO2_BEG(task<int>, (g), aw::optional<int> v{};) {
    CO2_AWAIT_SET(v, g.next());
    if (!v || *v != 1) CO2_RETURN(-1);
    CO2_AWAIT_SET(v, g.next()); // 重抛 "gen"
    CO2_RETURN(-2);
}
CO2_END

// 只取前两个，然后丢弃停在 yield 处的生成器。
auto take_two(gen<int> g) CO2_BEG(task<int>, (g), aw::optional<int> a{}; aw::optional<int> b{};) {
    CO2_AWAIT_SET(a, g.next());
    CO2_AWAIT_SET(b, g.next());
    CO2_RETURN(*a + *b);
}
CO2_END

template <class T> T run(aw::manual_executor& ctx, task<T> a) {
    auto f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

void sums() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, sum(numbers(5))) == 10);
    AW_CHECK(run(ctx, sum(async_numbers(ctx.get_executor(), 4))) == 60);
    AW_CHECK(run(ctx, sum(numbers(0))) == 0);
}

void rvalues_are_moved() { 
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, concat(words())) == "ab");
}

void body_exception_reaches_consumer() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, first_then_error(throws_after_one()), aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (std::runtime_error const& e) {
        AW_CHECK(std::string{e.what()} == "gen");
    }
}

void destroy_at_yield_and_unstarted() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, take_two(numbers(100))) == 1);
    { gen<int> unstarted = numbers(3); }
}

void generator_shares_consumer_executor() {
    aw::manual_executor ctx;
    bool same = false;
    AW_CHECK(run(ctx, sum(executor_probe(ctx.get_executor(), &same))) == 0);
    AW_CHECK(same);
}

// §9.3：稳态下一次 yield 零分配。
auto count_yields(gen<int> g, long* news) CO2_BEG(task<int>, (g, news), aw::optional<int> v{}; int n{}; long before{};) {
    CO2_AWAIT_SET(v, g.next());
    before = aw_test::new_calls().load();
    for (;;) {
        CO2_AWAIT_SET(v, g.next());
        if (!v) break;
        ++n;
    }
    *news = aw_test::new_calls().load() - before;
    CO2_RETURN(n);
}
CO2_END

void yield_does_not_allocate() {
    aw::manual_executor ctx;
    long news = -1;
    AW_CHECK(run(ctx, count_yields(numbers(1001), &news)) == 1000);
    AW_CHECK(news == 0);
}

} // namespace

int main() {
    sums();
    rvalues_are_moved();
    body_exception_reaches_consumer();
    destroy_at_yield_and_unstarted();
    generator_shares_consumer_executor();
    yield_does_not_allocate();
    return 0;
}
