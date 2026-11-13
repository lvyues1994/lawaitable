// §15.1 的 asio 190a8d8 对照（C++20 原生协程）。同线程项用单线程 io_context，跨线程项用
// asio::thread_pool(1) 加一个外部完成线程。项名与 aw_bench.cpp 相同。

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/concurrent_channel.hpp>

#include <cstdio>
#include <exception>
#include <tuple>
#include <utility>
#include <variant>

#include "bench_common.hpp"

namespace {

using namespace asio::experimental::awaitable_operators;
volatile long sink = 0;

asio::awaitable<int> leaf(int v) { co_return v; }

asio::awaitable<int> chain(int depth) {
    if (depth == 0) co_return 0;
    int v = co_await chain(depth - 1);
    co_return v + 1;
}

void create_destroy(long n) {
    for (long i = 0; i != bench::warmup; ++i) { auto a = leaf(1); }
    bench::window w{"create_destroy"};
    w.start();
    for (long i = 0; i != n; ++i) { auto a = leaf(static_cast<int>(i)); }
    w.stop(n);
}

asio::awaitable<void> await_child(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = sink + co_await leaf(1);
    bench::window w{"await_child"};
    w.start();
    for (long i = 0; i != n; ++i) sink = sink + co_await leaf(static_cast<int>(i));
    w.stop(n);
}

asio::awaitable<void> await_nested(long n) {
    for (long i = 0; i != 10; ++i) sink = co_await chain(1000);
    bench::window w{"await_child_nested_1000"};
    w.start();
    for (long i = 0; i != n; ++i) sink = co_await chain(1000);
    w.stop(n);
}

asio::awaitable<void> bridge_same(long n) {
    auto ex = co_await asio::this_coro::executor;
    for (long i = 0; i != bench::warmup; ++i) co_await asio::post(ex, asio::use_awaitable);
    bench::window w{"bridge_same_thread"};
    w.start();
    for (long i = 0; i != n; ++i) co_await asio::post(ex, asio::use_awaitable);
    w.stop(n);
}

template <class Handler> struct cross_job {
    Handler handler;
    void operator()() {
        auto ex = asio::get_associated_executor(handler);
        asio::dispatch(ex, std::move(handler));
    }
};
template <class Token> auto async_cross(bench::completer& c, Token&& token) {
    return asio::async_initiate<Token, void()>(
        [&c](auto handler) { c.submit(cross_job<decltype(handler)>{std::move(handler)}); }, token);
}

asio::awaitable<void> bridge_cross(bench::completer& c, long n) {
    for (long i = 0; i != bench::warmup; ++i) co_await async_cross(c, asio::use_awaitable);
    bench::window w{"bridge_cross_thread"};
    w.start();
    for (long i = 0; i != n; ++i) co_await async_cross(c, asio::use_awaitable);
    w.stop(n);
}

struct spawn_chain {
    asio::io_context* io;
    long* left;
    bench::window* w;
    long total;
    void operator()(std::exception_ptr, int v) const {
        sink = sink + v;
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        asio::co_spawn(*io, leaf(1), *this);
    }
};

struct strand_chain {
    asio::strand<asio::io_context::executor_type> s;
    long* left;
    bench::window* w;
    long total;
    void operator()() const {
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        asio::post(s, *this);
    }
};

asio::awaitable<void> or_ready(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = static_cast<long>((co_await (leaf(1) || leaf(2))).index());
    bench::window w{"or_ready"};
    w.start();
    for (long i = 0; i != n; ++i) sink = static_cast<long>((co_await (leaf(1) || leaf(2))).index());
    w.stop(n);
}

asio::awaitable<void> and_ready(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = std::get<0>(co_await (leaf(1) && leaf(2)));
    bench::window w{"and_ready"};
    w.start();
    for (long i = 0; i != n; ++i) sink = std::get<0>(co_await (leaf(1) && leaf(2)));
    w.stop(n);
}

// 增补 C9（只报告）：channel 往返，容量 1。channel 不是线程安全的；concurrent_channel 带锁（与 aw::channel 对应）。
template <class Channel> asio::awaitable<void> chan_sender(Channel& ch, long total) {
    for (long i = 0; i != total; ++i) co_await ch.async_send(asio::error_code{}, static_cast<int>(i), asio::use_awaitable);
}

template <class Channel> asio::awaitable<void> chan_receiver(Channel& ch, long n, char const* name) {
    int v = 0;
    for (long i = 0; i != bench::warmup; ++i) v = co_await ch.async_receive(asio::use_awaitable);
    bench::window w{name};
    w.start();
    for (long i = 0; i != n; ++i) v = co_await ch.async_receive(asio::use_awaitable);
    w.stop(n);
    sink = v;
}

template <class A> void run_on(asio::io_context& io, A a) {
    asio::co_spawn(io, std::move(a), asio::detached);
    io.restart();
    io.run();
}

} // namespace

int main() {
    asio::io_context io{1};
    asio::post(io, [] { create_destroy(2000000); });
    io.run();
    run_on(io, await_child(2000000));
    run_on(io, await_nested(2000));
    run_on(io, bridge_same(2000000));
    {
        asio::thread_pool pool{1};
        bench::completer c;
        asio::co_spawn(pool, bridge_cross(c, 100000), asio::use_future).get();
        pool.join();
    }
    {
        long left = bench::warmup;
        asio::co_spawn(io, leaf(1), spawn_chain{&io, &left, nullptr, 0});
        io.restart();
        io.run();
        long const n = 1000000;
        bench::window w{"co_spawn"};
        left = n;
        asio::post(io, [&] {
            w.start();
            asio::co_spawn(io, leaf(1), spawn_chain{&io, &left, &w, n});
        });
        io.restart();
        io.run();
    }
    {
        auto s = asio::make_strand(io.get_executor());
        long left = bench::warmup;
        asio::post(s, strand_chain{s, &left, nullptr, 0});
        io.restart();
        io.run();
        long const n = 2000000;
        bench::window w{"strand_post"};
        left = n;
        asio::post(io, [&] {
            w.start();
            asio::post(s, strand_chain{s, &left, &w, n});
        });
        io.restart();
        io.run();
    }
    run_on(io, or_ready(300000));
    run_on(io, and_ready(300000));
    {
        asio::experimental::channel<void(asio::error_code, int)> ch{io, 1};
        long const n = 1000000;
        asio::co_spawn(io, chan_sender(ch, bench::warmup + n), asio::detached);
        asio::co_spawn(io, chan_receiver(ch, n, "channel_ping_pong"), asio::detached);
        io.restart();
        io.run();
    }
    {
        asio::experimental::concurrent_channel<void(asio::error_code, int)> ch{io, 1};
        long const n = 1000000;
        asio::co_spawn(io, chan_sender(ch, bench::warmup + n), asio::detached);
        asio::co_spawn(io, chan_receiver(ch, n, "channel_ping_pong_concurrent"), asio::detached);
        io.restart();
        io.run();
    }
    return 0;
}
