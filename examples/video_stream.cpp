// 视频流示例：相机快（每 1 ms 一帧）、显示慢（每帧 4 ms），中间是容量 3 的 aw::channel。
// 依次用三种满队列策略跑一遍，打印收到 / 丢弃的帧和延迟：
//   block       相机被拖慢到显示的速度，不丢帧，延迟最多约 容量 × 处理时间；
//   drop_oldest 相机不受影响，显示的总是最新的帧，延迟低（实时预览）；
//   drop_newest 相机不受影响，保留的是队列里先到的帧，后来的被丢掉，延迟较高。
// 消费端演示两种写法：直接循环 async_receive，或者把 channel 包成 async_generator 再 next()。
// 作为 ctest 运行时只检查不变量（不检查具体时间）。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <awaitable.hpp>

namespace {

using pool_ex = aw::thread_pool::executor_type;
using clock_type = std::chrono::steady_clock;
template <class T> using pool_task = aw::awaitable<T, pool_ex>;

struct frame {
    int seq{-1};
    clock_type::time_point captured{};
    std::vector<unsigned char> pixels; // 真实场景里是帧缓冲；被丢弃时可以经 drop 回调还给缓冲池
};

using frame_channel = aw::channel<frame, pool_ex>;

constexpr int frame_count = 40;
constexpr std::size_t queue_capacity = 3;
constexpr auto capture_interval = std::chrono::milliseconds{1};
constexpr auto process_time = std::chrono::milliseconds{4};

struct stats {
    std::vector<int> seqs;
    double total_latency_ms{0};
    double max_latency_ms{0};

    void on_frame(frame const& f) {
        double const ms = std::chrono::duration<double, std::milli>(clock_type::now() - f.captured).count();
        seqs.push_back(f.seq);
        total_latency_ms += ms;
        if (ms > max_latency_ms) max_latency_ms = ms;
        // 模拟解码 / 渲染。示例里直接阻塞池线程；真实代码应把重活放到专门的执行上下文。
        std::this_thread::sleep_for(process_time);
    }
};

// 写法一：直接循环。as_tuple 让 channel_closed 作为普通返回值，而不是异常。
auto display_loop(frame_channel* ch, stats* st)
    CO2_BEG(pool_task<void>, (ch, st), std::tuple<std::error_code, frame> r{};) {
    for (;;) {
        CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
        if (std::get<0>(r)) break; // channel_closed：缓冲区已取尽
        st->on_frame(std::get<1>(r));
    }
    CO2_RETURN();
}
CO2_END

// 写法二：把 channel 包成拉式的 async_generator（生成器在消费者的执行线程上运行）。
auto frames_of(frame_channel* ch)
    CO2_BEG((aw::async_generator<frame, pool_ex>), (ch), std::tuple<std::error_code, frame> r{};) {
    for (;;) {
        CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
        if (std::get<0>(r)) break;
        CO2_YIELD(std::move(std::get<1>(r)));
    }
    CO2_RETURN();
}
CO2_END

auto display_generator(aw::async_generator<frame, pool_ex> frames, stats* st)
    CO2_BEG(pool_task<void>, (frames, st), aw::optional<frame> f{};) {
    for (;;) {
        CO2_AWAIT_SET(f, frames.next());
        if (!f) break;
        st->on_frame(*f);
    }
    CO2_RETURN();
}
CO2_END

char const* name_of(aw::channel_policy p) {
    switch (p) {
    case aw::channel_policy::block:
        return "block";
    case aw::channel_policy::drop_oldest:
        return "drop_oldest";
    case aw::channel_policy::drop_newest:
        return "drop_newest";
    }
    return "?";
}

std::string summarize(std::vector<int> const& seqs) {
    std::string out;
    for (std::size_t i = 0; i < seqs.size();) {
        std::size_t j = i;
        while (j + 1 < seqs.size() && seqs[j + 1] == seqs[j] + 1) ++j;
        if (!out.empty()) out += ",";
        out += std::to_string(seqs[i]);
        if (j != i) out += "-" + std::to_string(seqs[j]);
        i = j + 1;
    }
    return out;
}

bool check(bool ok, char const* what) {
    if (!ok) std::fprintf(stderr, "  invariant violated: %s\n", what);
    return ok;
}

bool run(aw::channel_policy policy) {
    aw::thread_pool pool{2};
    frame_channel ch{pool, queue_capacity, policy};
    int recycled = 0;
    ch.set_drop_handler([&recycled](frame&& f) {
        f.pixels.clear(); // 还给缓冲池
        ++recycled;
    });

    stats st;
    std::future<void> done =
        policy == aw::channel_policy::block
            ? aw::co_spawn(pool, display_loop(&ch, &st), aw::use_future)
            : aw::co_spawn(pool, display_generator(frames_of(&ch), &st), aw::use_future);

    // 相机线程：不是库线程。block 用 async_send + use_future 等空位；drop_* 用 try_send，从不等待。
    clock_type::time_point const start = clock_type::now();
    std::thread camera([&] {
        for (int i = 0; i != frame_count; ++i) {
            frame f{i, clock_type::now(), std::vector<unsigned char>(64U, static_cast<unsigned char>(i))};
            if (policy == aw::channel_policy::block) {
                ch.async_send(std::move(f), aw::use_future).get();
            } else {
                static_cast<void>(ch.try_send(std::move(f)));
            }
            std::this_thread::sleep_for(capture_interval);
        }
        ch.close();
    });
    camera.join();
    double const camera_ms = std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
    done.get();

    std::size_t const received = st.seqs.size();
    std::printf("%-12s camera %6.1f ms | received %2zu dropped %2llu | latency avg %5.1f ms max %5.1f ms | frames %s\n",
                name_of(policy), camera_ms, received, static_cast<unsigned long long>(ch.dropped()),
                received != 0U ? st.total_latency_ms / static_cast<double>(received) : 0.0, st.max_latency_ms,
                summarize(st.seqs).c_str());

    bool ok = true;
    bool ordered = true;
    for (std::size_t i = 1; i < received; ++i) ordered = ordered && st.seqs[i] > st.seqs[i - 1];
    ok &= check(ordered, "frames arrive in capture order");
    ok &= check(received + ch.dropped() == static_cast<std::uint64_t>(frame_count), "received + dropped == captured");
    ok &= check(static_cast<std::uint64_t>(recycled) == ch.dropped(), "every dropped frame reached the drop handler");
    switch (policy) {
    case aw::channel_policy::block:
        ok &= check(ch.dropped() == 0U && received == static_cast<std::size_t>(frame_count), "block never drops");
        break;
    case aw::channel_policy::drop_oldest:
        ok &= check(received != 0U && st.seqs.back() == frame_count - 1, "drop_oldest delivers the newest frame");
        break;
    case aw::channel_policy::drop_newest:
        ok &= check(received != 0U && st.seqs.front() == 0, "drop_newest keeps the earliest frames");
        break;
    }
    return ok;
}

} // namespace

int main() {
    std::printf("camera every %lld ms, display %lld ms per frame, queue capacity %zu, %d frames\n",
                static_cast<long long>(capture_interval.count()), static_cast<long long>(process_time.count()),
                queue_capacity, frame_count);
    bool ok = true;
    ok &= run(aw::channel_policy::block);
    ok &= run(aw::channel_policy::drop_oldest);
    ok &= run(aw::channel_policy::drop_newest);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
