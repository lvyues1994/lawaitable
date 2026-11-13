#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#include "awaitable/exec/thread_pool.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_coro.hpp"

namespace {

class latch {
  public:
    explicit latch(int count) : count_{count} {}
    void count_down() {
        std::lock_guard<std::mutex> lock{mutex_};
        if (--count_ == 0) cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock{mutex_};
        cv_.wait(lock, [this] { return count_ == 0; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
};

void postedFunctionsAllRun() {
    aw::thread_pool pool{4};
    auto ex = pool.get_executor();
    std::atomic<int> sum{0};
    latch done{1000};
    for (int i = 0; i != 1000; ++i) {
        ex.post([&, i] {
            sum.fetch_add(i);
            done.count_down();
        });
    }
    done.wait();
    AW_CHECK(sum.load() == 999 * 1000 / 2);
}

void runningInThisThreadAndDispatch() {
    aw::thread_pool pool{2};
    auto ex = pool.get_executor();
    AW_CHECK(!ex.running_in_this_thread());
    std::vector<std::string> trace;
    latch done{1};
    ex.post([&] {
        AW_CHECK(ex.running_in_this_thread());
        ex.dispatch([&] { trace.push_back("inline"); });
        trace.push_back("after");
        done.count_down();
    });
    done.wait();
    AW_CHECK((trace == std::vector<std::string>{"inline", "after"}));
}

auto hopOnto(aw::thread_pool& pool, bool& onWorker, latch& done)
    CO2_BEG(aw_test::eager_coro, (pool, onWorker, done)) {
    CO2_AWAIT(aw::schedule_on(pool));
    onWorker = pool.running_in_this_thread();
    done.count_down();
    CO2_RETURN();
}
CO2_END

void scheduleResumesOnAWorker() {
    aw::thread_pool pool{2};
    bool onWorker = false;
    latch done{1};
    hopOnto(pool, onWorker, done);
    done.wait();
    AW_CHECK(onWorker);
}

void queuedWorkIsRunOrDestroyedOnShutdown() {
    auto token = std::make_shared<int>(0);
    {
        aw::thread_pool pool{1};
        auto ex = pool.get_executor();
        std::shared_ptr<int> copy = token;
        ex.post([ex, copy] { ex.post([copy] {}); });
    }
    AW_CHECK(token.use_count() == 1);
}

void workerChainsReuseOperationMemory() {
    aw::thread_pool pool{1};
    auto ex = pool.get_executor();
    latch done{1};
    long news = -1;
    struct step {
        aw::thread_pool::executor_type ex;
        int remaining;
        aw_test::allocation_counter* counter;
        long* news;
        latch* done;
        void operator()() {
            if (remaining == 900) counter = new aw_test::allocation_counter{};
            if (remaining == 0) {
                *news = counter->news();
                delete counter;
                done->count_down();
                return;
            }
            ex.post(step{ex, remaining - 1, counter, news, done});
        }
    };
    ex.post(step{ex, 1000, nullptr, &news, &done});
    done.wait();
    if (aw_test::counting_new_enabled) AW_CHECK(news == 0);
}

// 验收修订（跨线程项）：工作线程按本线程可用的 CPU 数决定停车前要不要 pause 自旋。
// 只有 1 个 CPU 时自旋只会占住提交方要用的那个核，直接进入让出时间片的轮次。
void spinFollowsAvailableCpus() {
    AW_CHECK(!aw::detail::pause_spin_useful(1U));
    AW_CHECK(aw::detail::pause_spin_useful(2U));
    AW_CHECK(aw::detail::cpus_available_to_this_thread() >= 1U);
#if defined(__linux__)
    cpu_set_t original;
    CPU_ZERO(&original);
    AW_CHECK(sched_getaffinity(0, sizeof original, &original) == 0);
    AW_CHECK(aw::detail::cpus_available_to_this_thread() == static_cast<unsigned>(CPU_COUNT(&original)));
    std::size_t first = 0;
    while (!CPU_ISSET(first, &original)) ++first;
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(first, &one);
    AW_CHECK(sched_setaffinity(0, sizeof one, &one) == 0);
    AW_CHECK(aw::detail::cpus_available_to_this_thread() == 1U);
    {
        // 工作线程继承创建者的亲和性，在工作线程上看到的也是 1。
        aw::thread_pool pool{1U};
        latch done{1};
        std::atomic<unsigned> seen{0U};
        pool.get_executor().post([&] {
            seen = aw::detail::cpus_available_to_this_thread();
            done.count_down();
        });
        done.wait();
        AW_CHECK(seen.load() == 1U);
    }
    AW_CHECK(sched_setaffinity(0, sizeof original, &original) == 0);
#endif
}

} // namespace

int main() {
    postedFunctionsAllRun();
    runningInThisThreadAndDispatch();
    scheduleResumesOnAWorker();
    queuedWorkIsRunOrDestroyedOnShutdown();
    workerChainsReuseOperationMemory();
    spinFollowsAvailableCpus();
    AW_CHECK(aw::thread_pool::default_thread_count() >= 1U);
    return 0;
}
