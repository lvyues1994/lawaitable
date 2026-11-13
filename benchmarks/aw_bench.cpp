// §15.1 的本库基准（C++14）。aw-bench 打开回收缓存，aw-bench-nocache 定义 AW_DISABLE_RECYCLING_ALLOCATOR。
// 全部测量都在库线程上进行（manual_executor::run 期间或 thread_pool 工作线程上）。

#include <awaitable.hpp>

#include <cstdio>
#include <system_error>
#include <tuple>
#include <utility>

#include "bench_common.hpp"

namespace {

using namespace aw::awaitable_operators;
using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using pool_ex = aw::thread_pool::executor_type;

volatile long sink = 0;

auto leaf(int v) CO2_BEG(task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto chain(int depth) CO2_BEG(task<int>, (depth), int v{};) {
    if (depth == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(v, chain(depth - 1));
    CO2_RETURN(v + 1);
}
CO2_END

auto numbers(int n) CO2_BEG((aw::async_generator<int, ex_t>), (n), int i{};) {
    for (i = 0; i != n; ++i) CO2_YIELD(i);
    CO2_RETURN();
}
CO2_END

// 1. 创建与销毁（不运行）
void create_destroy(long n) {
    for (long i = 0; i != bench::warmup; ++i) { task<int> a = leaf(1); }
    bench::window w{"create_destroy"};
    w.start();
    for (long i = 0; i != n; ++i) { task<int> a = leaf(static_cast<int>(i)); }
    w.stop(n);
}

// 2. 等待子协程：单层与 1000 层
auto await_child(long n) CO2_BEG(task<void>, (n), long i{}; int v{}; bench::window w{"await_child"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(v, leaf(1));
    w.start();
    for (i = 0; i != n; ++i) {
        CO2_AWAIT_SET(v, leaf(static_cast<int>(i)));
        sink = sink + v;
    }
    w.stop(n);
    CO2_RETURN();
}
CO2_END

auto await_nested(long n) CO2_BEG(task<void>, (n), long i{}; int v{}; bench::window w{"await_child_nested_1000"};) {
    for (i = 0; i != 10; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.stop(n);
    sink = v;
    CO2_RETURN();
}
CO2_END

// 3. 异步桥接：同线程（完成方在同一个 manual_executor 上 post）
auto bridge_same(ex_t ex, long n) CO2_BEG(task<void>, (ex, n), long i{}; bench::window w{"bridge_same_thread"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 3'. 异步桥接：外部线程完成，回到 thread_pool
template <class Handler> struct cross_job {
    Handler handler;
    void operator()() {
        auto target = aw::get_associated_executor(handler, 0);
        target.dispatch(std::move(handler));
    }
};
struct initiate_cross {
    bench::completer* c;
    template <class Handler> void operator()(Handler&& h) const {
        c->submit(cross_job<typename std::decay<Handler>::type>{std::forward<Handler>(h)});
    }
};
template <class Token> auto async_cross(bench::completer& c, Token&& token) {
    return aw::async_initiate<Token, void()>(initiate_cross{&c}, token);
}

auto bridge_cross(bench::completer* c, long n)
    CO2_BEG((aw::awaitable<void, pool_ex>), (c, n), long i{}; bench::window w{"bridge_cross_thread"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT(async_cross(*c, aw::use_awaitable));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT(async_cross(*c, aw::use_awaitable));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 4. co_spawn + 完成回调（回调里发起下一次，同一时刻只有一个执行线程）
struct spawn_chain {
    aw::manual_executor* ctx;
    long* left;
    bench::window* w;
    long total;
    void operator()(std::exception_ptr, int v) const {
        sink = sink + v;
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        aw::co_spawn(*ctx, leaf(1), *this);
    }
};

// 5. strand 投递（任务里投递下一个）
struct strand_chain {
    aw::strand<ex_t> s;
    long* left;
    bench::window* w;
    long total;
    void operator()() const {
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        s.post(*this);
    }
};

// 6. async_generator yield 1000 次
auto generator_1000(long n)
    CO2_BEG(task<void>, (n), long i{}; aw::async_generator<int, ex_t> g{}; aw::optional<int> v{};
            bench::window w{"generator_yield_1000"};) {
    for (i = -10; i != n; ++i) {
        if (i == 0) w.start();
        g = numbers(1000);
        for (;;) {
            CO2_AWAIT_SET(v, g.next());
            if (!v) break;
            sink = sink + *v;
        }
    }
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 7. || 与 &&（两个就绪的操作数）
auto or_ready(long n) CO2_BEG(task<void>, (n), long i{}; aw::variant<int, int> r{}; bench::window w{"or_ready"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(r, leaf(1) || leaf(2));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(r, leaf(1) || leaf(2));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

auto and_ready(long n) CO2_BEG(task<void>, (n), long i{}; std::tuple<int, int> r{}; bench::window w{"and_ready"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(r, leaf(1) && leaf(2));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(r, leaf(1) && leaf(2));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 增补 C9（只报告）：channel 往返。同一 manual_executor 上一个发送协程、一个接收协程，容量 1。
using int_channel = aw::channel<int, ex_t>;

auto chan_sender(int_channel* ch, long total) CO2_BEG(task<void>, (ch, total), long i{};) {
    for (i = 0; i != total; ++i) CO2_AWAIT(ch->async_send(static_cast<int>(i), aw::use_awaitable));
    CO2_RETURN();
}
CO2_END

auto chan_receiver(int_channel* ch, long n) CO2_BEG(task<void>, (ch, n), long i{}; int v{}; bench::window w{"channel_ping_pong"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(v, ch->async_receive(aw::use_awaitable));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(v, ch->async_receive(aw::use_awaitable));
    w.stop(n);
    sink = v;
    CO2_RETURN();
}
CO2_END

template <class A> void run_on(aw::manual_executor& ctx, A a) {
    aw::co_spawn(ctx, std::move(a), aw::detached);
    ctx.run();
}

} // namespace

int main() {
    bench::size_probe("awaitable_frame_base<manual>", sizeof(aw::detail::awaitable_frame_base<ex_t>));
    bench::size_probe("awaitable_frame<int,manual>", sizeof(aw::detail::awaitable_frame<int, ex_t>));
    bench::size_probe("entry_frame_impl<manual,int,spawn_chain>",
                      sizeof(aw::detail::entry_frame_impl<ex_t, int, spawn_chain>));
    bench::size_probe("async_generator next_awaiter", sizeof(aw::async_generator<int, ex_t>::next_awaiter));

    aw::manual_executor ctx;
    ctx.get_executor().post([] { create_destroy(2000000); });
    ctx.run();
    run_on(ctx, await_child(2000000));
    run_on(ctx, await_nested(2000));
    run_on(ctx, bridge_same(ctx.get_executor(), 2000000));
    {
        aw::thread_pool pool{1U};
        bench::completer c;
        aw::co_spawn(pool, bridge_cross(&c, 100000), aw::use_future).get();
    }
    {
        long left = bench::warmup;
        aw::co_spawn(ctx, leaf(1), spawn_chain{&ctx, &left, nullptr, 0});
        ctx.run();
        long const n = 1000000;
        bench::window w{"co_spawn"};
        left = n;
        ctx.get_executor().post([&] {
            w.start();
            aw::co_spawn(ctx, leaf(1), spawn_chain{&ctx, &left, &w, n});
        });
        ctx.run();
    }
    {
        aw::strand<ex_t> s = aw::make_strand(ctx.get_executor());
        long left = bench::warmup;
        s.post(strand_chain{s, &left, nullptr, 0});
        ctx.run();
        long const n = 2000000;
        bench::window w{"strand_post"};
        left = n;
        ctx.get_executor().post([&] {
            w.start();
            s.post(strand_chain{s, &left, &w, n});
        });
        ctx.run();
    }
    run_on(ctx, generator_1000(2000));
    run_on(ctx, or_ready(300000));
    run_on(ctx, and_ready(300000));
    {
        int_channel ch{ctx, 1};
        long const n = 1000000;
        aw::co_spawn(ctx, chan_sender(&ch, bench::warmup + n), aw::detached);
        aw::co_spawn(ctx, chan_receiver(&ch, n), aw::detached);
        ctx.run();
    }
    return 0;
}
