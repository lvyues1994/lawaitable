// 规格增补 C7：打开回收缓存后，同一线程上的发送 / 接收循环稳态每次操作全局 operator new 为 0。
// TSan 构建下 counting_new 不计数，只做行为检查。
#include <exception>
#include <system_error>
#include <tuple>

#include "awaitable/as_tuple.hpp"
#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/channel.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/use_awaitable.hpp"
#include "check.hpp"
#include "counting_new.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using int_channel = aw::channel<int, ex_t>;

constexpr int warmup = 1000;
constexpr int measured = 1000;

struct window {
    long start{0};
    long end{0};
};

auto sender(int_channel* ch) CO2_BEG(task<void>, (ch), int i{};) {
    for (i = 0; i != warmup + measured; ++i) CO2_AWAIT(ch->async_send(int{i}, aw::use_awaitable));
    CO2_RETURN();
}
CO2_END

auto receiver(int_channel* ch, window* w) CO2_BEG(task<int>, (ch, w), int i{}; int v{}; int bad{};) {
    for (i = 0; i != warmup + measured; ++i) {
        if (i == warmup) w->start = aw_test::new_calls().load();
        CO2_AWAIT_SET(v, ch->async_receive(aw::use_awaitable));
        if (v != i) ++bad;
    }
    w->end = aw_test::new_calls().load();
    CO2_RETURN(bad);
}
CO2_END

void ping_pong(std::size_t capacity, aw::channel_policy policy, bool with_slot) {
    aw::manual_executor ctx;
    int_channel ch{ctx, capacity, policy};
    window w;
    int bad = -1;
    aw::cancellation_signal s1, s2;
    if (with_slot) {
        aw::co_spawn(ctx, sender(&ch), aw::bind_cancellation_slot(s1.slot(), [](std::exception_ptr e) { AW_CHECK(!e); }));
        aw::co_spawn(ctx, receiver(&ch, &w), aw::bind_cancellation_slot(s2.slot(), [&](std::exception_ptr e, int b) {
                         AW_CHECK(!e);
                         bad = b;
                     }));
    } else {
        aw::co_spawn(ctx, sender(&ch), [](std::exception_ptr e) { AW_CHECK(!e); });
        aw::co_spawn(ctx, receiver(&ch, &w), [&](std::exception_ptr e, int b) {
            AW_CHECK(!e);
            bad = b;
        });
    }
    ctx.run();
    AW_CHECK(bad == 0);
    if (aw_test::counting_new_enabled) {
        if (w.end - w.start != 0)
            std::fprintf(stderr, "capacity %zu policy %d slot %d: %ld allocations\n", capacity,
                         static_cast<int>(policy), with_slot ? 1 : 0, w.end - w.start);
        AW_CHECK(w.end - w.start == 0);
    }
}

} // namespace

int main() {
    ping_pong(1, aw::channel_policy::block, false);
    ping_pong(0, aw::channel_policy::block, false);
    ping_pong(4, aw::channel_policy::block, false);
    ping_pong(1, aw::channel_policy::block, true); // 等待节点 + slot 处理器
    ping_pong(0, aw::channel_policy::block, true);
    return 0;
}
