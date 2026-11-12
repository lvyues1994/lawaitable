#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "counting_new.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;

auto value(int v) CO2_BEG(task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto plus_one(int v) CO2_BEG(task<int>, (v), int r{};) {
    CO2_AWAIT_SET(r, value(v));
    CO2_RETURN(r + 1);
}
CO2_END

auto thrower(std::string message) CO2_BEG(task<int>, (message)) {
    throw std::runtime_error{message};
}
CO2_END

auto rethrows_child() CO2_BEG(task<int>, (), int r{};) {
    CO2_AWAIT_SET(r, thrower("deep"));
    CO2_RETURN(r);
}
CO2_END

auto level3(std::vector<std::string>& t) CO2_BEG(task<void>, (t)) {
    t.push_back("3");
    CO2_RETURN();
}
CO2_END

auto level2(std::vector<std::string>& t) CO2_BEG(task<void>, (t)) {
    t.push_back("2-begin");
    CO2_AWAIT(level3(t));
    t.push_back("2-end");
    CO2_RETURN();
}
CO2_END

auto level1(std::vector<std::string>& t) CO2_BEG(task<void>, (t)) {
    t.push_back("1-begin");
    CO2_AWAIT(level2(t));
    t.push_back("1-end");
    CO2_RETURN();
}
CO2_END

auto deep(int n) CO2_BEG(task<int>, (n), int r{};) {
    if (n == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(r, deep(n - 1));
    CO2_RETURN(r + 1);
}
CO2_END

struct not_default {
    explicit not_default(int x) : v{x} {}
    int v;
};

auto make_not_default(int v) CO2_BEG(task<not_default>, (v)) { CO2_RETURN(not_default{v}); }
CO2_END

auto runs_on_its_executor() CO2_BEG(task<bool>, (), ex_t ex{};) {
    CO2_AWAIT_SET(ex, aw::this_coro::executor);
    CO2_RETURN(ex.running_in_this_thread());
}
CO2_END

auto holds(std::shared_ptr<int> p) CO2_BEG(task<int>, (p)) { CO2_RETURN(*p); }
CO2_END

auto child_loop(int n, long* news)
    CO2_BEG(task<int>, (n, news), int i{}; int sum{}; int r{}; aw_test::allocation_counter* counter{};) {
    for (i = 0; i != n; ++i) {
        if (i == 100) counter = new aw_test::allocation_counter{};
        CO2_AWAIT_SET(r, value(i));
        sum += r;
    }
    *news = counter->news();
    delete counter;
    CO2_RETURN(sum);
}
CO2_END

void spawnDeliversValue() {
    aw::manual_executor ctx;
    int got = 0;
    bool called = false;
    aw::co_spawn(ctx, plus_one(41), [&](std::exception_ptr e, int v) {
        AW_CHECK(!e);
        got = v;
        called = true;
    });
    AW_CHECK(!called); // 不在 run 里：co_spawn 先 dispatch，即排队
    ctx.run();
    AW_CHECK(called && got == 42);
}

void nestedFramesRunInOrder() {
    aw::manual_executor ctx;
    std::vector<std::string> t;
    bool done = false;
    aw::co_spawn(ctx.get_executor(), level1(t), [&](std::exception_ptr e) {
        AW_CHECK(!e);
        done = true;
    });
    ctx.run();
    AW_CHECK(done);
    AW_CHECK((t == std::vector<std::string>{"1-begin", "2-begin", "3", "2-end", "1-end"}));
}

void exceptionsTravelToTheHandler() {
    aw::manual_executor ctx;
    std::string message;
    int v = -1;
    aw::co_spawn(ctx, rethrows_child(), [&](std::exception_ptr e, int value) {
        v = value;
        try {
            std::rethrow_exception(e);
        } catch (std::runtime_error const& error) {
            message = error.what();
        }
    });
    ctx.run();
    AW_CHECK(message == "deep" && v == 0);
}

void deepRecursionIsIterative() {
    aw::manual_executor ctx;
    int got = 0;
    aw::co_spawn(ctx, deep(10000), [&](std::exception_ptr, int v) { got = v; });
    ctx.run();
    AW_CHECK(got == 10000);
}

void nonDefaultConstructibleResultUsesOptional() {
    aw::manual_executor ctx;
    int got = 0;
    aw::co_spawn(ctx, make_not_default(7), [&](std::exception_ptr e, aw::optional<not_default> v) {
        AW_CHECK(!e && v.has_value());
        got = v->v;
    });
    ctx.run();
    AW_CHECK(got == 7);
}

void thisCoroExecutor() {
    aw::manual_executor ctx;
    bool on_it = false;
    aw::co_spawn(ctx, runs_on_its_executor(), [&](std::exception_ptr, bool b) { on_it = b; });
    ctx.run();
    AW_CHECK(on_it);
}

void launchTimeCompletionIsNotDeliveredInline() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    bool delivered = false;
    bool checked = false;
    ex.post([&] {
        aw::co_spawn(ex, value(1), [&](std::exception_ptr, int) { delivered = true; });
        AW_CHECK(!delivered); // §7.3 第 4 条：启动期同步完成，交付先 post
        checked = true;
    });
    ctx.run();
    AW_CHECK(checked && delivered);
}

void detachedRethrowsFromRun() {
    aw::manual_executor ctx;
    aw::co_spawn(ctx, thrower("detached"), aw::detached);
    bool caught = false;
    try {
        ctx.run();
    } catch (std::runtime_error const& e) {
        caught = std::string{e.what()} == "detached";
    }
    AW_CHECK(caught);
}

void handlerExceptionsEscapeRun() {
    aw::manual_executor ctx;
    aw::co_spawn(ctx, value(1), [](std::exception_ptr, int) { throw std::logic_error{"handler"}; });
    bool caught = false;
    try {
        ctx.run();
    } catch (std::logic_error const&) {
        caught = true;
    }
    AW_CHECK(caught);
}

void useFutureToken() {
    aw::manual_executor ctx;
    std::future<int> f = aw::co_spawn(ctx, plus_one(1), aw::use_future);
    ctx.run();
    AW_CHECK(f.get() == 2);
}

void abandonedThreadsAreUnwound() {
    auto p = std::make_shared<int>(5);
    {
        aw::manual_executor ctx;
        aw::co_spawn(ctx, holds(p), [](std::exception_ptr, int) { AW_CHECK(false); });
        AW_CHECK(p.use_count() == 2);
    } // 没有 run：launch 操作被销毁，执行线程经 executor 展开（关闭期间内联）
    AW_CHECK(p.use_count() == 1);
}

void childFramesReuseRecycledMemory() {
    aw::manual_executor ctx;
    long news = -1;
    int sum = 0;
    aw::co_spawn(ctx, child_loop(1100, &news), [&](std::exception_ptr e, int s) {
        AW_CHECK(!e);
        sum = s;
    });
    ctx.run();
    AW_CHECK(sum == 1099 * 1100 / 2);
    if (aw_test::counting_new_enabled) AW_CHECK(news == 0);
}

auto pool_plus_one(int v) CO2_BEG(aw::pool_awaitable<int>, (v)) { CO2_RETURN(v + 1); }
CO2_END

void runsOnThreadPool() {
    aw::thread_pool pool{2};
    std::future<int> f = aw::co_spawn(pool, pool_plus_one(9), aw::use_future);
    AW_CHECK(f.get() == 10);
}

} // namespace

int main() {
    spawnDeliversValue();
    nestedFramesRunInOrder();
    exceptionsTravelToTheHandler();
    deepRecursionIsIterative();
    nonDefaultConstructibleResultUsesOptional();
    thisCoroExecutor();
    launchTimeCompletionIsNotDeliveredInline();
    detachedRethrowsFromRun();
    handlerExceptionsEscapeRun();
    useFutureToken();
    abandonedThreadsAreUnwound();
    childFramesReuseRecycledMemory();
    runsOnThreadPool();
    return 0;
}
