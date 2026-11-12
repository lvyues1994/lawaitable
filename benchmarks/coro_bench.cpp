// §15.2：coro（补丁后的测试夹具 co2::Task / spawn / whenAll / AsyncGenerator）对照，C++14。

#include <cstdio>
#include <tuple>

#include "co2/async_generator.hpp"
#include "co2/manual_executor.hpp"
#include "co2/spawn.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "co2/when_all.hpp"

#include "bench_common.hpp"

namespace {

volatile long sink = 0;

auto leaf(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto chain(int depth) CO2_BEG(co2::Task<int>, (depth), int v{};) {
    if (depth == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(v, chain(depth - 1));
    CO2_RETURN(v + 1);
}
CO2_END

auto numbers(int n) CO2_BEG(co2::AsyncGenerator<int>, (n), int i{};) {
    for (i = 0; i != n; ++i) CO2_YIELD(i);
    CO2_RETURN();
}
CO2_END

auto await_child(long n) CO2_BEG(co2::Task<int>, (n), long i{}; int v{}; bench::window w{"await_child"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(v, leaf(1));
    w.start();
    for (i = 0; i != n; ++i) {
        CO2_AWAIT_SET(v, leaf(static_cast<int>(i)));
        sink = sink + v;
    }
    w.stop(n);
    CO2_RETURN(0);
}
CO2_END

auto await_nested(long n) CO2_BEG(co2::Task<int>, (n), long i{}; int v{}; bench::window w{"await_child_nested_1000"};) {
    for (i = 0; i != 10; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.stop(n);
    CO2_RETURN(v);
}
CO2_END

auto generator_1000(long n) CO2_BEG(co2::Task<int>, (n), long i{}; co2::AsyncGenerator<int> g{numbers(0)}; bool has{};
                                    bench::window w{"generator_yield_1000"};) {
    for (i = -10; i != n; ++i) {
        if (i == 0) w.start();
        g = numbers(1000);
        for (;;) {
            CO2_AWAIT_SET(has, g.next());
            if (!has) break;
            sink = sink + g.value();
        }
    }
    w.stop(n);
    CO2_RETURN(0);
}
CO2_END

auto and_ready(long n) CO2_BEG(co2::Task<int>, (n), long i{}; std::tuple<int, int> r{}; bench::window w{"and_ready"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(r, co2::whenAll(leaf(1), leaf(2)));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(r, co2::whenAll(leaf(1), leaf(2)));
    w.stop(n);
    CO2_RETURN(std::get<0>(r));
}
CO2_END

} // namespace

int main() {
    for (long i = 0; i != bench::warmup; ++i) { auto a = leaf(1); }
    {
        long const n = 2000000;
        bench::window w{"create_destroy"};
        w.start();
        for (long i = 0; i != n; ++i) { auto a = leaf(static_cast<int>(i)); }
        w.stop(n);
    }
    sink = co2::syncWait(await_child(2000000));
    sink = co2::syncWait(await_nested(2000));
    {
        co2::ManualExecutor executor;
        for (long i = 0; i != bench::warmup; ++i) {
            auto h = co2::spawn(executor, leaf(1));
            executor.run();
            sink = h.join();
        }
        long const n = 1000000;
        bench::window w{"co_spawn"};
        w.start();
        for (long i = 0; i != n; ++i) {
            auto h = co2::spawn(executor, leaf(1));
            executor.run();
            sink = h.join();
        }
        w.stop(n);
    }
    sink = co2::syncWait(generator_1000(2000));
    sink = co2::syncWait(and_ready(300000));
    return 0;
}
