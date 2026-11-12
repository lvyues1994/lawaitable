// §15.2：同编译器下的最小原生 C++20 task（对称转移），只用于第 1、2 项。

#include <coroutine>
#include <cstdio>
#include <exception>
#include <utility>

#include "bench_common.hpp"

namespace {

volatile long sink = 0;

template <class T> struct task {
    struct promise_type {
        T value{};
        std::coroutine_handle<> continuation{std::noop_coroutine()};
        task get_return_object() { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct final_awaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        final_awaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = v; }
        void unhandled_exception() { std::terminate(); }
    };
    explicit task(std::coroutine_handle<promise_type> h) : handle{h} {}
    task(task&& o) noexcept : handle{std::exchange(o.handle, {})} {}
    ~task() {
        if (handle) handle.destroy();
    }
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
        handle.promise().continuation = c;
        return handle;
    }
    T await_resume() { return handle.promise().value; }
    std::coroutine_handle<promise_type> handle;
};

task<int> leaf(int v) { co_return v; }

task<int> chain(int depth) {
    if (depth == 0) co_return 0;
    int v = co_await chain(depth - 1);
    co_return v + 1;
}

task<int> await_child(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = sink + co_await leaf(1);
    bench::window w{"await_child"};
    w.start();
    for (long i = 0; i != n; ++i) sink = sink + co_await leaf(static_cast<int>(i));
    w.stop(n);
    co_return 0;
}

task<int> await_nested(long n) {
    for (long i = 0; i != 10; ++i) sink = co_await chain(1000);
    bench::window w{"await_child_nested_1000"};
    w.start();
    for (long i = 0; i != n; ++i) sink = co_await chain(1000);
    w.stop(n);
    co_return 0;
}

void run(task<int> t) { t.handle.resume(); }

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
    run(await_child(2000000));
    run(await_nested(2000));
    return 0;
}
