#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/exec/thread_pool.hpp"
#include "check.hpp"

namespace {

void tasksNeverOverlapAndKeepFifoPerProducer() {
    aw::thread_pool pool{4};
    auto s = aw::make_strand(pool.get_executor());
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    int counter = 0; // 只在 strand 上访问，不加锁
    std::vector<int> order[4];
    std::mutex done_mutex;
    std::condition_variable done_cv;
    int finished = 0;
    std::vector<std::thread> producers;
    for (int p = 0; p != 4; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i != 2500; ++i) {
                s.post([&, p, i] {
                    if (inside.fetch_add(1) != 0) overlap = true;
                    ++counter;
                    order[p].push_back(i);
                    inside.fetch_sub(1);
                    if (counter == 10000) {
                        std::lock_guard<std::mutex> lock{done_mutex};
                        finished = 1;
                        done_cv.notify_all();
                    }
                });
            }
        });
    }
    for (auto& t : producers) t.join();
    std::unique_lock<std::mutex> lock{done_mutex};
    done_cv.wait(lock, [&] { return finished == 1; });
    AW_CHECK(!overlap.load());
    AW_CHECK(counter == 10000);
    for (auto const& o : order) {
        AW_CHECK(o.size() == 2500U);
        for (std::size_t i = 0; i != o.size(); ++i) AW_CHECK(o[i] == static_cast<int>(i));
    }
}

void runningInThisThreadAndDispatch() {
    aw::manual_executor ctx;
    auto s = aw::make_strand(ctx.get_executor());
    auto other = aw::make_strand(ctx.get_executor());
    std::vector<int> trace;
    AW_CHECK(!s.running_in_this_thread());
    s.dispatch([&] { trace.push_back(0); }); // 不在 strand 上：排队
    AW_CHECK(trace.empty());
    s.post([&] {
        AW_CHECK(s.running_in_this_thread());
        AW_CHECK(!other.running_in_this_thread());
        s.dispatch([&] { trace.push_back(1); }); // 在 strand 上：内联
        trace.push_back(2);
    });
    ctx.get_executor().post([&] { AW_CHECK(!s.running_in_this_thread()); });
    ctx.run();
    AW_CHECK((trace == std::vector<int>{0, 1, 2}));
}

void exceptionsLeaveTheStrandUsable() {
    aw::manual_executor ctx;
    auto s = aw::make_strand(ctx.get_executor());
    int ran = 0;
    s.post([] { throw std::runtime_error{"boom"}; });
    s.post([&] { ++ran; });
    bool caught = false;
    try {
        ctx.run();
    } catch (std::runtime_error const&) {
        caught = true;
    }
    AW_CHECK(caught);
    ctx.run();
    AW_CHECK(ran == 1);
    s.post([&] { ++ran; });
    ctx.run();
    AW_CHECK(ran == 2);
}

void equalityAndAccessors() {
    aw::manual_executor ctx;
    auto a = aw::make_strand(ctx.get_executor());
    auto copy = a;
    auto b = aw::make_strand(ctx.get_executor());
    AW_CHECK(a == copy && a != b);
    AW_CHECK(a.get_inner_executor() == ctx.get_executor());
    AW_CHECK(&a.context() == &ctx);
    static_assert(aw::is_executor<aw::strand<aw::manual_executor::executor_type>>::value, "");
}

void destroyingTheLastHandleDestroysPendingWork() {
    int destroyed = 0;
    struct counted {
        int* destroyed;
        bool moved{false};
        counted(int* d) : destroyed{d} {}
        counted(counted&& other) noexcept : destroyed{other.destroyed} { other.moved = true; }
        ~counted() {
            if (!moved) ++*destroyed;
        }
        void operator()() const {}
    };
    {
        aw::manual_executor ctx;
        auto s = aw::make_strand(ctx.get_executor());
        s.post(counted{&destroyed});
        s.post(counted{&destroyed});
    }
    AW_CHECK(destroyed == 2);
}

} // namespace

int main() {
    tasksNeverOverlapAndKeepFifoPerProducer();
    runningInThisThreadAndDispatch();
    exceptionsLeaveTheStrandUsable();
    equalityAndAccessors();
    destroyingTheLastHandleDestroysPendingWork();
    return 0;
}
