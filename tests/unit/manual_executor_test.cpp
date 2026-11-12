#include <stdexcept>
#include <string>
#include <vector>

#include "awaitable/exec/manual_executor.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_coro.hpp"

namespace {

auto hop(aw::scheduler& target, std::vector<std::string>& trace)
    CO2_BEG(aw_test::eager_coro, (target, trace)) {
    trace.push_back("before");
    CO2_AWAIT(aw::schedule_on(target));
    trace.push_back("after");
    CO2_RETURN();
}
CO2_END

void postRunsLaterInFifoOrder() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::vector<int> order;
    ex.post([&] { order.push_back(1); });
    ex.post([&] { order.push_back(2); });
    AW_CHECK(order.empty() && ctx.pending() == 2U);
    AW_CHECK(ctx.run() == 2U);
    AW_CHECK((order == std::vector<int>{1, 2}));
    AW_CHECK(!ctx.run_one());
}

void dispatchIsInlineOnlyInsideRun() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::vector<std::string> trace;
    ex.dispatch([&] { trace.push_back("outside"); });
    AW_CHECK(trace.empty());
    ex.post([&] {
        AW_CHECK(ex.running_in_this_thread());
        ex.dispatch([&] { trace.push_back("inline"); });
        trace.push_back("after-dispatch");
    });
    AW_CHECK(!ex.running_in_this_thread());
    ctx.run();
    AW_CHECK((trace == std::vector<std::string>{"outside", "inline", "after-dispatch"}));
}

void pollRunsOnlyReadyItems() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    int ran = 0;
    ex.post([&] {
        ++ran;
        ex.post([&] { ++ran; });
    });
    AW_CHECK(ctx.poll() == 1U && ran == 1);
    AW_CHECK(ctx.poll() == 1U && ran == 2);
}

void exceptionsPropagateToTheCaller() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    int ran = 0;
    ex.post([] { throw std::runtime_error{"boom"}; });
    ex.post([&] { ++ran; });
    bool caught = false;
    try {
        ctx.run();
    } catch (std::runtime_error const& e) {
        caught = std::string{e.what()} == "boom";
    }
    AW_CHECK(caught && ran == 0 && ctx.pending() == 1U);
    AW_CHECK(!ex.running_in_this_thread());
    ctx.run();
    AW_CHECK(ran == 1);
}

void scheduleResumesCoreCoroutines() {
    aw::manual_executor ctx;
    std::vector<std::string> trace;
    hop(ctx, trace);
    AW_CHECK((trace == std::vector<std::string>{"before"}));
    ctx.run();
    AW_CHECK((trace == std::vector<std::string>{"before", "after"}));
}

void shutdownDestroysQueuedOperations() {
    int destroyed = 0;
    struct counted {
        int* destroyed;
        bool moved{false};
        counted(int* d) : destroyed{d} {}
        counted(counted&& other) noexcept : destroyed{other.destroyed} { other.moved = true; }
        ~counted() {
            if (!moved) ++*destroyed;
        }
        void operator()() const { AW_CHECK(false); }
    };
    {
        aw::manual_executor ctx;
        ctx.get_executor().post(counted{&destroyed});
    }
    AW_CHECK(destroyed == 1);
}

void operationsReuseRecycledMemoryInsideRun() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    int remaining = 1000;
    long news_in_steady_state = -1;
    aw_test::allocation_counter* counter = nullptr;
    struct step {
        aw::manual_executor::executor_type ex;
        int* remaining;
        long* news;
        aw_test::allocation_counter** counter;
        void operator()() {
            if (*remaining == 900) *counter = new aw_test::allocation_counter{};
            if (--*remaining == 0) {
                *news = (*counter)->news();
                delete *counter;
                return;
            }
            ex.post(step{ex, remaining, news, counter});
        }
    };
    ex.post(step{ex, &remaining, &news_in_steady_state, &counter});
    ctx.run();
    AW_CHECK(remaining == 0);
    if (aw_test::counting_new_enabled) AW_CHECK(news_in_steady_state == 0);
}

void equality() {
    aw::manual_executor a;
    aw::manual_executor b;
    AW_CHECK(a.get_executor() == a.get_executor());
    AW_CHECK(a.get_executor() != b.get_executor());
    AW_CHECK(&a.get_executor().context() == &a);
    static_assert(aw::is_executor<aw::manual_executor::executor_type>::value, "executor");
    static_assert(!aw::is_executor<aw::manual_executor>::value, "context is not an executor");
}

} // namespace

int main() {
    postRunsLaterInFifoOrder();
    dispatchIsInlineOnlyInsideRun();
    pollRunsOnlyReadyItems();
    exceptionsPropagateToTheCaller();
    scheduleResumesCoreCoroutines();
    shutdownDestroysQueuedOperations();
    operationsReuseRecycledMemoryInsideRun();
    equality();
    return 0;
}
