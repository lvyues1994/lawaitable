// §14.6：thread_pool、co_spawn 跨线程完成、strand、|| / && 的取消竞争、取消与完成同时到达、
// sync_wait 两种顺序；channel 的多生产者多消费者、外部线程丢帧、取消与发送竞争。用法：aw-stress-test <轮数>。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <awaitable.hpp>

#include "awaitable/core/detail/move_only_function.hpp"
#include "check.hpp"
#include "test_ops.hpp"

namespace {

using namespace aw::awaitable_operators;
using pool_ex = aw::thread_pool::executor_type;
using strand_ex = aw::strand<pool_ex>;
template <class T> using pool_task = aw::awaitable<T, pool_ex>;
template <class T> using strand_task = aw::awaitable<T, strand_ex>;

// 等 n 个回调。
class latch {
  public:
    explicit latch(int n) : remaining_{n} {}
    void count_down() {
        if (remaining_.fetch_sub(1) == 1) done_.set_value();
    }
    void wait() { done_.get_future().wait(); }

  private:
    std::atomic<int> remaining_;
    std::promise<void> done_;
};

// ---- 1. thread_pool：大量执行线程，每个让出两次 ----
auto hop_twice(pool_ex ex, std::atomic<int>* sum) CO2_BEG(pool_task<void>, (ex, sum)) {
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    sum->fetch_add(1);
    CO2_RETURN();
}
CO2_END

void thread_pool_round() {
    aw::thread_pool pool{4U};
    std::atomic<int> sum{0};
    constexpr int n = 2000;
    latch done{n};
    for (int i = 0; i != n; ++i)
        aw::co_spawn(pool, hop_twice(pool.get_executor(), &sum), [&](std::exception_ptr e) {
            AW_CHECK(!e);
            done.count_down();
        });
    done.wait();
    AW_CHECK(sum.load() == n);
}

// ---- 2. co_spawn 跨线程完成：操作在外部线程完成，回到池 ----
auto foreign(aw_test::foreign_threads* threads, pool_ex ex, int v) CO2_BEG(pool_task<int>, (threads, ex, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    AW_CHECK(ex.running_in_this_thread());
    CO2_RETURN(r);
}
CO2_END

void cross_thread_round() {
    aw::thread_pool pool{4U};
    std::atomic<int> sum{0};
    constexpr int n = 64;
    latch done{n};
    {
        aw_test::foreign_threads threads;
        for (int i = 0; i != n; ++i)
            aw::co_spawn(pool, foreign(&threads, pool.get_executor(), i), [&](std::exception_ptr e, int v) {
                AW_CHECK(!e);
                sum.fetch_add(v);
                done.count_down();
            });
        done.wait();
    }
    AW_CHECK(sum.load() == n * (n - 1) / 2);
}

// ---- 3. strand：多个执行线程交替等待，互不重叠 ----
auto strand_worker(strand_ex s, int rounds, std::atomic<int>* inside, int* counter)
    CO2_BEG(strand_task<void>, (s, rounds, inside, counter), int k{};) {
    for (k = 0; k != rounds; ++k) {
        AW_CHECK(inside->fetch_add(1) == 0);
        ++*counter; // 只在 strand 上访问：TSan 能抓到违背串行化的情况
        inside->fetch_sub(1);
        CO2_AWAIT(aw::post(s, aw::use_awaitable));
    }
    CO2_RETURN();
}
CO2_END

void strand_round() {
    aw::thread_pool pool{4U};
    strand_ex s = aw::make_strand(pool.get_executor());
    std::atomic<int> inside{0};
    int counter = 0;
    constexpr int workers = 6;
    latch done{workers};
    for (int i = 0; i != workers; ++i)
        aw::co_spawn(s, strand_worker(s, 200, &inside, &counter), [&](std::exception_ptr e) {
            AW_CHECK(!e);
            done.count_down();
        });
    done.wait();
    std::promise<int> seen;
    s.post([&] { seen.set_value(counter); });
    AW_CHECK(seen.get_future().get() == workers * 200);
}

// ---- 4. || / && 的取消竞争（组合跑在 strand 上，§6.2 第 7 条）----
auto strand_delay(strand_ex s, int n) CO2_BEG(strand_task<int>, (s, n), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(s, aw::use_awaitable));
    CO2_RETURN(n);
}
CO2_END

auto strand_fail(strand_ex s, int n) CO2_BEG(strand_task<int>, (s, n), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(s, aw::use_awaitable));
    throw std::runtime_error{"fail"};
}
CO2_END

auto race(strand_ex s, int i) CO2_BEG(strand_task<int>, (s, i), aw::variant<int, int> v{}; std::tuple<int, int> t{};) {
    CO2_AWAIT_SET(v, strand_delay(s, i % 3) || strand_delay(s, (i + 1) % 3));
    CO2_AWAIT_SET(t, strand_delay(s, 1) && strand_delay(s, i % 2));
    CO2_RETURN(static_cast<int>(v.index()) + std::get<0>(t));
}
CO2_END

auto race_fail(strand_ex s, int i) CO2_BEG(strand_task<int>, (s, i), std::tuple<int, int> t{};) {
    CO2_AWAIT_SET(t, strand_fail(s, i % 2) && strand_delay(s, 2));
    CO2_RETURN(-1);
}
CO2_END

void operators_round() {
    aw::thread_pool pool{4U};
    constexpr int n = 300;
    latch done{2 * n};
    std::atomic<int> failures{0};
    for (int i = 0; i != n; ++i) {
        strand_ex s = aw::make_strand(pool.get_executor());
        aw::co_spawn(s, race(s, i), [&](std::exception_ptr e, int v) {
            AW_CHECK(!e && v >= 1 && v <= 2);
            done.count_down();
        });
        aw::co_spawn(s, race_fail(s, i), [&](std::exception_ptr e, int) {
            if (e) failures.fetch_add(1);
            done.count_down();
        });
    }
    done.wait();
    AW_CHECK(failures.load() == n);
}

// ---- 5. 取消与完成同时到达：先到者生效，回调恰好一次 ----
struct racy_state {
    explicit racy_state(strand_ex ex) : s{std::move(ex)} {}
    strand_ex s;
    co2::detail::MoveOnlyFunction<void(std::error_code, int)> handler;
    aw::cancellation_slot slot;
};

struct racy_cancel {
    std::shared_ptr<racy_state> st;
    void operator()(aw::cancellation_type) {
        // 在 strand 上（co_spawn 的父 slot 处理器先 dispatch 过来）。处理器正在运行，不能清 slot。
        if (!st->handler) return;
        auto h = std::move(st->handler);
        st->slot = aw::cancellation_slot{};
        st->s.post(aw_test::bind_completion(std::move(h), std::error_code{aw::error::operation_aborted}, 0));
    }
};

struct racy_complete {
    std::shared_ptr<racy_state> st;
    void operator()() {
        if (!st->handler) return; // 已被取消
        if (st->slot.is_connected()) st->slot.clear();
        auto h = std::move(st->handler);
        h(std::error_code{}, 1);
    }
};

struct initiate_racy {
    aw_test::foreign_threads* threads;
    template <class Handler> void operator()(Handler&& handler) const {
        using handler_type = typename std::decay<Handler>::type;
        strand_ex target = handler.get_executor(); // 只配合 use_awaitable：回调就是执行线程
        auto st = std::make_shared<racy_state>(target);
        aw::cancellation_slot slot = aw::get_associated_cancellation_slot(handler);
        st->handler = co2::detail::MoveOnlyFunction<void(std::error_code, int)>(handler_type(std::forward<Handler>(handler)));
        if (slot.is_connected()) {
            st->slot = slot;
            slot.template emplace<racy_cancel>(racy_cancel{st});
        }
        threads->start([st] { st->s.dispatch(racy_complete{st}); });
    }
};

template <class Token> auto async_racy(aw_test::foreign_threads& threads, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_racy{&threads}, token);
}

auto racy_body(aw_test::foreign_threads* threads) CO2_BEG(strand_task<int>, (threads), int v{};) {
    CO2_AWAIT_SET(v, async_racy(*threads, aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

void cancel_vs_complete_round() {
    aw::thread_pool pool{4U};
    constexpr int n = 64;
    std::atomic<int> calls{0};
    std::atomic<int> aborted{0};
    latch done{n};
    {
        aw_test::foreign_threads threads;
        std::vector<std::unique_ptr<aw::cancellation_signal>> signals;
        for (int i = 0; i != n; ++i) {
            strand_ex s = aw::make_strand(pool.get_executor());
            signals.emplace_back(new aw::cancellation_signal);
            aw::co_spawn(s, racy_body(&threads),
                         aw::bind_cancellation_slot(signals.back()->slot(), [&](std::exception_ptr e, int) {
                             calls.fetch_add(1);
                             if (e) aborted.fetch_add(1);
                             done.count_down();
                         }));
        }
        // co_spawn 装的父 slot 处理器可以在任何线程 emit（§11）。
        for (auto& sig : signals) sig->emit(aw::cancellation_type::terminal);
        done.wait();
    }
    AW_CHECK(calls.load() == n);
    AW_CHECK(aborted.load() >= 0 && aborted.load() <= n);
}

// ---- 6. sync_wait 两种顺序 ----
auto ready(int v) CO2_BEG(pool_task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto later(aw_test::foreign_threads* threads, int v) CO2_BEG(pool_task<int>, (threads, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

void sync_wait_round() {
    aw::thread_pool pool{2U};
    long sum = 0;
    for (int i = 0; i != 500; ++i) sum += aw::sync_wait(pool.get_executor(), ready(i));
    AW_CHECK(sum == 500L * 499L / 2L);
    aw_test::foreign_threads threads;
    for (int i = 0; i != 20; ++i) AW_CHECK(aw::sync_wait(pool.get_executor(), later(&threads, i)) == i);
}


// ---- 7. channel：多生产者多消费者（block 策略，容量 0 / 1 / 8）----
using int_channel = aw::channel<int, pool_ex>;

auto chan_producer(int_channel* ch, int base, int n, std::atomic<int>* live)
    CO2_BEG(pool_task<void>, (ch, base, n, live), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(ch->async_send(base + i, aw::use_awaitable));
    if (live->fetch_sub(1) == 1) ch->close();
    CO2_RETURN();
}
CO2_END

auto chan_consumer(int_channel* ch, pool_ex ex, std::atomic<long>* sum, std::atomic<int>* count)
    CO2_BEG(pool_task<void>, (ch, ex, sum, count), std::tuple<std::error_code, int> r{};) {
    for (;;) {
        CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
        AW_CHECK(ex.running_in_this_thread());
        if (std::get<0>(r)) break;
        sum->fetch_add(std::get<1>(r));
        count->fetch_add(1);
    }
    AW_CHECK(std::get<0>(r) == aw::channel_errc::channel_closed);
    CO2_RETURN();
}
CO2_END

void channel_mpmc_round() {
    for (std::size_t capacity : {std::size_t{0}, std::size_t{1}, std::size_t{8}}) {
        aw::thread_pool pool{4U};
        int_channel ch{pool, capacity};
        constexpr int producers = 4, consumers = 4, per = 300;
        std::atomic<int> live{producers};
        std::atomic<long> sum{0};
        std::atomic<int> count{0};
        latch done{producers + consumers};
        auto cb = [&](std::exception_ptr e) {
            AW_CHECK(!e);
            done.count_down();
        };
        for (int c = 0; c != consumers; ++c)
            aw::co_spawn(pool, chan_consumer(&ch, pool.get_executor(), &sum, &count), cb);
        for (int p = 0; p != producers; ++p) aw::co_spawn(pool, chan_producer(&ch, p * per, per, &live), cb);
        done.wait();
        constexpr int total = producers * per;
        AW_CHECK(count.load() == total);
        AW_CHECK(sum.load() == static_cast<long>(total) * (total - 1) / 2);
    }
}

// ---- 8. channel：外部线程 try_send（drop_oldest / drop_newest），池上单消费者 ----
auto chan_ordered_consumer(int_channel* ch, int* received, int* last, bool* ordered)
    CO2_BEG(pool_task<void>, (ch, received, last, ordered), std::tuple<std::error_code, int> r{};) {
    for (;;) {
        CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
        if (std::get<0>(r)) break;
        if (std::get<1>(r) <= *last) *ordered = false;
        *last = std::get<1>(r);
        ++*received;
    }
    CO2_RETURN();
}
CO2_END

void channel_drop_foreign_round() {
    for (aw::channel_policy policy : {aw::channel_policy::drop_oldest, aw::channel_policy::drop_newest}) {
        aw::thread_pool pool{2U};
        int_channel ch{pool, 4, policy};
        int received = 0, last = -1;
        bool ordered = true;
        std::future<void> f = aw::co_spawn(pool, chan_ordered_consumer(&ch, &received, &last, &ordered), aw::use_future);
        constexpr int n = 3000;
        std::thread camera([&] {
            for (int i = 0; i != n; ++i) AW_CHECK(ch.try_send(int{i}));
            ch.close();
        });
        f.get();
        camera.join();
        AW_CHECK(ordered);
        AW_CHECK(static_cast<std::uint64_t>(received) + ch.dropped() == static_cast<std::uint64_t>(n));
        if (policy == aw::channel_policy::drop_oldest) AW_CHECK(last == n - 1); // 最新的帧总会送到
    }
}

// ---- 9. channel：slot 取消与外部线程的发送竞争；不丢失、不重复 ----
// 每个接收方在自己的 strand 上：emit（经 co_spawn 的取消桥 dispatch 到协程的 executor）与恢复时清 slot
// 必须串行（§6.2 第 7 条），多线程池本身不提供这一点。
auto chan_cancellable_receiver(int_channel* ch, std::atomic<long>* sum, std::atomic<int>* got, std::atomic<int>* aborted)
    CO2_BEG(strand_task<void>, (ch, sum, got, aborted), std::tuple<std::error_code, int> r{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_total_cancellation{}));
    CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
    if (!std::get<0>(r)) {
        sum->fetch_add(std::get<1>(r));
        got->fetch_add(1);
    } else {
        AW_CHECK(std::get<0>(r) == aw::error::operation_aborted);
        aborted->fetch_add(1);
    }
    CO2_RETURN();
}
CO2_END

void channel_cancel_race_round() {
    aw::thread_pool pool{4U};
    constexpr int n = 200;
    int_channel ch{pool, static_cast<std::size_t>(n)};
    std::atomic<long> sum{0};
    std::atomic<int> got{0}, aborted{0};
    latch done{n};
    std::vector<std::unique_ptr<aw::cancellation_signal>> signals;
    for (int i = 0; i != n; ++i) {
        signals.emplace_back(new aw::cancellation_signal);
        aw::co_spawn(aw::make_strand(pool.get_executor()), chan_cancellable_receiver(&ch, &sum, &got, &aborted),
                     aw::bind_cancellation_slot(signals.back()->slot(), [&](std::exception_ptr e) {
                         AW_CHECK(!e);
                         done.count_down();
                     }));
    }
    std::thread sender([&] {
        for (int i = 0; i != n; ++i) AW_CHECK(ch.try_send(int{i}));
    });
    for (int i = 0; i != n; i += 2) signals[static_cast<std::size_t>(i)]->emit(aw::cancellation_type::total);
    sender.join();
    // 发送 n 个值、n 个接收方：没被取消的接收方一定拿到值，所以所有接收方都会结束。
    // 守恒：拿到值的接收方个数 + 缓冲区剩余 = n，且和不变（不丢失、不重复）。
    done.wait();
    long rest = 0;
    int left = 0;
    while (auto v = ch.try_receive()) {
        rest += *v;
        ++left;
    }
    AW_CHECK(got.load() + aborted.load() == n);
    AW_CHECK(got.load() + left == n);
    AW_CHECK(sum.load() + rest == static_cast<long>(n) * (n - 1) / 2);
}

} // namespace

int main(int argc, char** argv) {
    int const rounds = argc > 1 ? std::atoi(argv[1]) : 10;
    AW_CHECK(rounds > 0);
    struct item {
        char const* name;
        void (*fn)();
    };
    item const items[] = {{"thread_pool", &thread_pool_round},       {"cross_thread", &cross_thread_round},
                          {"strand", &strand_round},                 {"operators", &operators_round},
                          {"cancel_vs_complete", &cancel_vs_complete_round}, {"sync_wait", &sync_wait_round},
                          {"channel_mpmc", &channel_mpmc_round},     {"channel_drop_foreign", &channel_drop_foreign_round},
                          {"channel_cancel_race", &channel_cancel_race_round}};
    for (item const& it : items) {
        for (int r = 0; r != rounds; ++r) it.fn();
        std::printf("%s: %d rounds ok\n", it.name, rounds);
    }
    return 0;
}
