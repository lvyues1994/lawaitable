// 有界 channel（规格增补 C2–C6）。manual_executor 上的确定性用例。
#include <atomic>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "awaitable/as_tuple.hpp"
#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/channel.hpp"
#include "awaitable/channel_error.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/error.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/exec/thread_pool.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using int_channel = aw::channel<int, ex_t>;

std::error_code const closed_ec = aw::channel_errc::channel_closed;
std::error_code const cancelled_ec = aw::channel_errc::channel_cancelled;
std::error_code const aborted_ec = aw::error::operation_aborted;

// 原始回调：记录完成参数。
struct recv_record {
    int calls{0};
    std::error_code ec{};
    int value{-1};
};
struct recv_cb {
    recv_record* r;
    void operator()(std::error_code ec, int v) const {
        ++r->calls;
        r->ec = ec;
        r->value = v;
    }
};
struct send_record {
    int calls{0};
    std::error_code ec{};
};
struct send_cb {
    send_record* r;
    void operator()(std::error_code ec) const {
        ++r->calls;
        r->ec = ec;
    }
};

std::vector<int> drain(int_channel& ch) {
    std::vector<int> out;
    while (auto v = ch.try_receive()) out.push_back(*v);
    return out;
}

void error_codes() {
    std::error_code ec = aw::make_error_code(aw::channel_errc::channel_closed);
    AW_CHECK(std::string{ec.category().name()} == "aw.channel");
    AW_CHECK(ec == aw::channel_errc::channel_closed);
    AW_CHECK(ec != aw::channel_errc::channel_cancelled);
    AW_CHECK(!ec.message().empty());
    AW_CHECK(aw::make_error_code(aw::channel_errc::channel_cancelled).message() != ec.message());
}

void accessors() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 3};
    AW_CHECK(ch.capacity() == 3U);
    AW_CHECK(ch.policy() == aw::channel_policy::block);
    AW_CHECK(ch.get_executor() == ctx.get_executor());
    AW_CHECK(ch.is_open());
    AW_CHECK(ch.size() == 0U);
    AW_CHECK(ch.dropped() == 0U);
    int_channel ch2{ctx.get_executor(), 1, aw::channel_policy::drop_newest};
    AW_CHECK(ch2.policy() == aw::channel_policy::drop_newest);
}

void buffered_fifo_try_ops() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 3};
    AW_CHECK(ch.try_send(1) && ch.try_send(2) && ch.try_send(3));
    int keep = 4;
    AW_CHECK(!ch.try_send(std::move(keep))); // 满了：block 策略不等待、不丢弃
    AW_CHECK(ch.size() == 3U && ch.dropped() == 0U);
    AW_CHECK((drain(ch) == std::vector<int>{1, 2, 3}));
    AW_CHECK(!ch.try_receive());
}

void move_only_value_not_taken_on_failure() {
    aw::manual_executor ctx;
    aw::channel<std::unique_ptr<int>, ex_t> ch{ctx, 1};
    AW_CHECK(ch.try_send(std::unique_ptr<int>{new int{1}}));
    std::unique_ptr<int> p{new int{2}};
    AW_CHECK(!ch.try_send(std::move(p)));
    AW_CHECK(p && *p == 2); // 失败时值不被移走
    auto got = ch.try_receive();
    AW_CHECK(got && *got && **got == 1);
}

// 完成总是 post 到回调的关联 executor：发起时不内联调用。
void completions_are_posted() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 2};
    send_record s;
    ch.async_send(5, send_cb{&s});
    AW_CHECK(s.calls == 0);
    recv_record r;
    ch.async_receive(recv_cb{&r});
    AW_CHECK(r.calls == 0);
    ctx.run();
    AW_CHECK(s.calls == 1 && !s.ec);
    AW_CHECK(r.calls == 1 && !r.ec && r.value == 5);
}

void waiting_receiver_gets_value_directly() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 2};
    recv_record r1, r2;
    ch.async_receive(recv_cb{&r1});
    ch.async_receive(recv_cb{&r2});
    ctx.run();
    AW_CHECK(r1.calls == 0 && r2.calls == 0);
    AW_CHECK(ch.try_send(7));
    AW_CHECK(ch.size() == 0U); // 没进缓冲区
    ctx.run();
    AW_CHECK(r1.calls == 1 && r1.value == 7 && r2.calls == 0); // 最早的接收方先得到
    AW_CHECK(ch.try_send(8));
    ctx.run();
    AW_CHECK(r2.calls == 1 && r2.value == 8);
}

void block_policy_sender_waits_until_space() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    send_record s1, s2;
    ch.async_send(1, send_cb{&s1});
    ch.async_send(2, send_cb{&s2});
    ctx.run();
    AW_CHECK(s1.calls == 1 && s2.calls == 0);
    auto v = ch.try_receive();
    AW_CHECK(v && *v == 1);
    AW_CHECK(ch.size() == 1U); // 等待的发送方的值补进了缓冲区
    ctx.run();
    AW_CHECK(s2.calls == 1 && !s2.ec);
    AW_CHECK((drain(ch) == std::vector<int>{2}));
}

void rendezvous_capacity_zero() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 0};
    AW_CHECK(!ch.try_send(1)); // 没有接收方
    send_record s;
    ch.async_send(9, send_cb{&s});
    ctx.run();
    AW_CHECK(s.calls == 0 && ch.size() == 0U);
    auto v = ch.try_receive();
    AW_CHECK(v && *v == 9);
    ctx.run();
    AW_CHECK(s.calls == 1 && !s.ec);
    // 接收方先等，发送方再来。
    recv_record r;
    ch.async_receive(recv_cb{&r});
    AW_CHECK(ch.try_send(10));
    ctx.run();
    AW_CHECK(r.calls == 1 && r.value == 10);
}

void drop_oldest_policy() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 2, aw::channel_policy::drop_oldest};
    std::vector<int> dropped;
    ch.set_drop_handler([&](int&& v) { dropped.push_back(v); });
    for (int i = 1; i <= 4; ++i) AW_CHECK(ch.try_send(int{i}));
    send_record s;
    ch.async_send(5, send_cb{&s}); // 满了也不等待
    ctx.run();
    AW_CHECK(s.calls == 1 && !s.ec);
    AW_CHECK(ch.dropped() == 3U);
    AW_CHECK((dropped == std::vector<int>{1, 2, 3}));
    AW_CHECK((drain(ch) == std::vector<int>{4, 5}));
}

void drop_newest_policy() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 2, aw::channel_policy::drop_newest};
    std::vector<int> dropped;
    ch.set_drop_handler([&](int&& v) { dropped.push_back(v); });
    for (int i = 1; i <= 4; ++i) AW_CHECK(ch.try_send(int{i})); // 被策略丢弃也算被接收
    send_record s;
    ch.async_send(5, send_cb{&s});
    ctx.run();
    AW_CHECK(s.calls == 1 && !s.ec);
    AW_CHECK(ch.dropped() == 3U);
    AW_CHECK((dropped == std::vector<int>{3, 4, 5}));
    AW_CHECK((drain(ch) == std::vector<int>{1, 2}));
}

void drop_policy_without_handler_destroys_values() {
    aw::manual_executor ctx;
    aw::channel<std::shared_ptr<int>, ex_t> ch{ctx, 1, aw::channel_policy::drop_oldest};
    std::shared_ptr<int> first = std::make_shared<int>(1);
    std::weak_ptr<int> watch = first;
    AW_CHECK(ch.try_send(std::move(first)));
    AW_CHECK(ch.try_send(std::make_shared<int>(2)));
    AW_CHECK(watch.expired());
    AW_CHECK(ch.dropped() == 1U);
}

void close_drains_then_reports_closed() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 4};
    AW_CHECK(ch.try_send(1) && ch.try_send(2));
    ch.close();
    ch.close(); // 幂等
    AW_CHECK(!ch.is_open());
    int v = 3;
    AW_CHECK(!ch.try_send(std::move(v)));
    send_record s;
    ch.async_send(3, send_cb{&s});
    recv_record r1, r2, r3;
    ch.async_receive(recv_cb{&r1});
    ch.async_receive(recv_cb{&r2});
    ch.async_receive(recv_cb{&r3});
    ctx.run();
    AW_CHECK(s.calls == 1 && s.ec == closed_ec);
    AW_CHECK(r1.value == 1 && !r1.ec && r2.value == 2 && !r2.ec);
    AW_CHECK(r3.calls == 1 && r3.ec == closed_ec && r3.value == 0);
    AW_CHECK(!ch.try_receive());
}

void close_completes_waiters() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    recv_record r;
    ch.async_receive(recv_cb{&r});
    ctx.run();
    ch.close();
    ctx.run();
    AW_CHECK(r.calls == 1 && r.ec == closed_ec);

    int_channel full{ctx, 1};
    AW_CHECK(full.try_send(1));
    send_record s;
    full.async_send(2, send_cb{&s});
    ctx.run();
    AW_CHECK(s.calls == 0);
    full.close();
    ctx.run();
    AW_CHECK(s.calls == 1 && s.ec == closed_ec);
    AW_CHECK((drain(full) == std::vector<int>{1})); // 等待中的发送值被丢弃，缓冲区保留
    AW_CHECK(full.dropped() == 0U);
}

void cancel_completes_waiters_and_keeps_open() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    recv_record r;
    ch.async_receive(recv_cb{&r});
    ctx.run();
    ch.cancel();
    ctx.run();
    AW_CHECK(r.calls == 1 && r.ec == cancelled_ec);
    AW_CHECK(ch.is_open());
    AW_CHECK(ch.try_send(1));
    send_record s;
    ch.async_send(2, send_cb{&s});
    ctx.run();
    ch.cancel();
    ctx.run();
    AW_CHECK(s.calls == 1 && s.ec == cancelled_ec);
    AW_CHECK((drain(ch) == std::vector<int>{1}));
}

// ---- 按操作取消（slot）----

void slot_cancel_waiting_receive_all_types() {
    aw::cancellation_type const types[] = {aw::cancellation_type::terminal, aw::cancellation_type::partial,
                                           aw::cancellation_type::total};
    for (aw::cancellation_type type : types) {
        aw::manual_executor ctx;
        int_channel ch{ctx, 1};
        aw::cancellation_signal sig;
        recv_record r;
        ch.async_receive(aw::bind_cancellation_slot(sig.slot(), recv_cb{&r}));
        ctx.run();
        AW_CHECK(sig.slot().has_handler()); // 进入等待才安装
        sig.emit(type);
        ctx.run();
        AW_CHECK(r.calls == 1 && r.ec == aborted_ec);
        AW_CHECK(ch.try_send(4)); // 被取消的接收方已出队：值进缓冲区
        AW_CHECK(ch.size() == 1U);
        sig.slot().clear();
    }
}

void slot_cancel_waiting_send() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    AW_CHECK(ch.try_send(1));
    aw::cancellation_signal sig;
    send_record s;
    ch.async_send(2, aw::bind_cancellation_slot(sig.slot(), send_cb{&s}));
    ctx.run();
    sig.emit(aw::cancellation_type::total);
    ctx.run();
    AW_CHECK(s.calls == 1 && s.ec == aborted_ec);
    AW_CHECK((drain(ch) == std::vector<int>{1})); // 被取消的发送没有入队
}

void immediate_completion_installs_no_handler() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    AW_CHECK(ch.try_send(1));
    aw::cancellation_signal sig;
    recv_record r;
    ch.async_receive(aw::bind_cancellation_slot(sig.slot(), recv_cb{&r}));
    AW_CHECK(!sig.slot().has_handler());
    ctx.run();
    AW_CHECK(r.calls == 1 && r.value == 1);
}

// 完成先发生、之后才 emit：处理器发现节点已出队，什么也不做。
void emit_after_completion_is_harmless() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    aw::cancellation_signal sig;
    recv_record r;
    ch.async_receive(aw::bind_cancellation_slot(sig.slot(), recv_cb{&r}));
    AW_CHECK(ch.try_send(3));
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(r.calls == 1 && !r.ec && r.value == 3);
    sig.slot().clear(); // 释放处理器持有的节点引用
}

// channel 在处理器清除之前销毁：已完成的节点不再引用 channel。
void handler_outlives_channel() {
    aw::manual_executor ctx;
    aw::cancellation_signal sig;
    recv_record r;
    {
        int_channel ch{ctx, 1};
        ch.async_receive(aw::bind_cancellation_slot(sig.slot(), recv_cb{&r}));
        AW_CHECK(ch.try_send(3));
        ctx.run();
    }
    sig.emit(aw::cancellation_type::terminal);
    AW_CHECK(r.calls == 1 && r.value == 3);
}

// ---- 协程 ----

auto producer(int_channel* ch, int n) CO2_BEG(task<int>, (ch, n), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(ch->async_send(int{i}, aw::use_awaitable));
    ch->close();
    CO2_RETURN(n);
}
CO2_END

auto consumer(int_channel* ch, std::vector<int>* got)
    CO2_BEG(task<int>, (ch, got), std::tuple<std::error_code, int> r{};) {
    for (;;) {
        CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
        if (std::get<0>(r)) break;
        got->push_back(std::get<1>(r));
    }
    CO2_RETURN(std::get<0>(r) == aw::channel_errc::channel_closed ? 1 : -1);
}
CO2_END

void coroutine_pipeline(std::size_t capacity) {
    aw::manual_executor ctx;
    int_channel ch{ctx, capacity};
    std::vector<int> got;
    int produced = 0, consumed = 0;
    aw::co_spawn(ctx, producer(&ch, 20), [&](std::exception_ptr e, int v) {
        AW_CHECK(!e);
        produced = v;
    });
    aw::co_spawn(ctx, consumer(&ch, &got), [&](std::exception_ptr e, int v) {
        AW_CHECK(!e);
        consumed = v;
    });
    ctx.run();
    AW_CHECK(produced == 20 && consumed == 1);
    AW_CHECK(got.size() == 20U);
    for (int i = 0; i != 20; ++i) AW_CHECK(got[static_cast<std::size_t>(i)] == i);
}

// use_awaitable 下 channel_closed 转成 system_error。
auto receive_throwing(int_channel* ch) CO2_BEG(task<int>, (ch), int v{};) {
    CO2_AWAIT_SET(v, ch->async_receive(aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

void closed_throws_with_use_awaitable() {
    aw::manual_executor ctx;
    int_channel ch{ctx, 1};
    ch.close();
    bool threw = false;
    aw::co_spawn(ctx, receive_throwing(&ch), [&](std::exception_ptr e, int) {
        try {
            if (e) std::rethrow_exception(e);
        } catch (std::system_error const& error) {
            threw = error.code() == closed_ec;
        }
    });
    ctx.run();
    AW_CHECK(threw);
}

// co_spawn 的取消经 slot 到达等待中的 async_receive。
auto receive_reporting(int_channel* ch) CO2_BEG(task<int>, (ch), std::tuple<std::error_code, int> r{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_total_cancellation{}));
    CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
    CO2_RETURN(std::get<0>(r) == aw::error::operation_aborted ? 1 : -1);
}
CO2_END

void coroutine_cancellation() {
    aw::cancellation_type const types[] = {aw::cancellation_type::terminal, aw::cancellation_type::partial,
                                           aw::cancellation_type::total};
    for (aw::cancellation_type type : types) {
        aw::manual_executor ctx;
        int_channel ch{ctx, 1};
        aw::cancellation_signal sig;
        int result = 0;
        aw::co_spawn(ctx, receive_reporting(&ch), aw::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e, int v) {
                         AW_CHECK(!e);
                         result = v;
                     }));
        ctx.run();
        AW_CHECK(result == 0);
        sig.emit(type);
        ctx.run();
        AW_CHECK(result == 1);
        AW_CHECK(ch.try_send(1) && ch.size() == 1U);
    }
}

// 不可默认构造的 T：receive_type 是 aw::optional<T>。
struct no_default {
    explicit no_default(int x) : v{x} {}
    no_default(no_default&&) = default;
    no_default& operator=(no_default&&) = default;
    int v;
};

void non_default_constructible_value() {
    static_assert(std::is_same<aw::channel<no_default, ex_t>::receive_type, aw::optional<no_default>>::value, "");
    static_assert(std::is_same<int_channel::receive_type, int>::value, "");
    aw::manual_executor ctx;
    aw::channel<no_default, ex_t> ch{ctx, 1};
    AW_CHECK(ch.try_send(no_default{5}));
    int got = 0;
    std::error_code last{};
    bool empty_on_close = false;
    ch.async_receive([&](std::error_code ec, aw::optional<no_default> v) {
        if (!ec && v) got = v->v;
    });
    ch.close();
    ch.async_receive([&](std::error_code ec, aw::optional<no_default> v) {
        last = ec;
        empty_on_close = !v;
    });
    ctx.run();
    AW_CHECK(got == 5 && last == closed_ec && empty_on_close);
}

void use_future_token() {
    aw::thread_pool pool{1};
    aw::channel<int, aw::thread_pool::executor_type> ch{pool, 0};
    std::future<void> sent = ch.async_send(1, aw::use_future); // rendezvous：等接收方
    std::future<int> received = ch.async_receive(aw::use_future);
    sent.get();
    AW_CHECK(received.get() == 1);

    // 错误路径：use_future 交付的是装着 system_error 的 exception_ptr。池线程在 set_exception 之后
    // 才析构 promise，可能比主线程的 catch 晚，于是由池线程释放异常对象。libstdc++ 的异常引用计数在
    // 未插桩的 libstdc++.so 里，TSan 看不到这条 happens-before，会把 catch 里读 e.code() 与池线程的
    // free 报成数据竞争（误报；纯 std::promise / std::thread 也能复现）。先销毁线程池（join）再 get，
    // 让池线程的全部动作 happens-before 主线程读异常，测试在任何标准库下都无竞争。
    std::future<int> after;
    {
        aw::thread_pool closing_pool{1};
        aw::channel<int, aw::thread_pool::executor_type> closed{closing_pool, 0};
        closed.close();
        after = closed.async_receive(aw::use_future);
    }
    bool threw = false;
    try {
        static_cast<void>(after.get());
    } catch (std::system_error const& e) {
        threw = e.code() == closed_ec;
    }
    AW_CHECK(threw);
}

// 析构时挂起的回调被销毁、不被调用。
struct tracked_cb {
    int* destroyed;
    int* called;
    std::shared_ptr<int> token;
    void operator()(std::error_code, int) { ++*called; }
};

void destroy_with_pending_operations() {
    aw::manual_executor ctx;
    int called = 0, destroyed = 0;
    std::shared_ptr<int> token = std::make_shared<int>(0);
    std::weak_ptr<int> watch = token;
    {
        int_channel ch{ctx, 1};
        ch.async_receive(tracked_cb{&destroyed, &called, std::move(token)});
        ctx.run();
        AW_CHECK(!watch.expired());
    }
    AW_CHECK(watch.expired());
    ctx.run();
    AW_CHECK(called == 0);
}

// strand 上的协程经 channel 恢复时仍在自己的 strand 上。
using pool_ex = aw::thread_pool::executor_type;
using strand_ex = aw::strand<pool_ex>;

auto strand_receiver(aw::channel<int, pool_ex>* ch, strand_ex s, std::atomic<int>* wrong)
    CO2_BEG((aw::awaitable<int, strand_ex>), (ch, s, wrong), int v{}; int sum{}; int i{};) {
    for (i = 0; i != 50; ++i) {
        CO2_AWAIT_SET(v, ch->async_receive(aw::use_awaitable));
        if (!s.running_in_this_thread()) wrong->fetch_add(1);
        sum += v;
    }
    CO2_RETURN(sum);
}
CO2_END

void strand_affinity() {
    aw::thread_pool pool{3};
    aw::channel<int, pool_ex> ch{pool, 2};
    strand_ex s = aw::make_strand(pool.get_executor());
    std::atomic<int> wrong{0};
    std::future<int> sum = aw::co_spawn(s, strand_receiver(&ch, s, &wrong), aw::use_future);
    std::thread feeder([&] {
        for (int i = 0; i != 50; ++i) ch.async_send(int{i}, aw::use_future).get();
    });
    AW_CHECK(sum.get() == 49 * 50 / 2);
    feeder.join();
    AW_CHECK(wrong.load() == 0);
}

} // namespace

int main() {
    error_codes();
    accessors();
    buffered_fifo_try_ops();
    move_only_value_not_taken_on_failure();
    completions_are_posted();
    waiting_receiver_gets_value_directly();
    block_policy_sender_waits_until_space();
    rendezvous_capacity_zero();
    drop_oldest_policy();
    drop_newest_policy();
    drop_policy_without_handler_destroys_values();
    close_drains_then_reports_closed();
    close_completes_waiters();
    cancel_completes_waiters_and_keeps_open();
    slot_cancel_waiting_receive_all_types();
    slot_cancel_waiting_send();
    immediate_completion_installs_no_handler();
    emit_after_completion_is_harmless();
    handler_outlives_channel();
    coroutine_pipeline(0);
    coroutine_pipeline(1);
    coroutine_pipeline(4);
    closed_throws_with_use_awaitable();
    coroutine_cancellation();
    non_default_constructible_value();
    use_future_token();
    destroy_with_pending_operations();
    strand_affinity();
    return 0;
}
