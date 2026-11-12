// 问题 03 的回归（规格 §14.1）：补丁 03 不改变分配次数，并且 syncWait 的两种完成顺序都正确。
//   syncWait(已就绪 Task)          每次 1 次分配（Task 帧）
//   syncWait(Task{whenAll(2)})     每次 5 次分配（父帧 + 2 个子帧 + 超槽的 awaiter + StopState）
//   spawn + run + join             每次 3 次分配（帧 + JoinState + StopState）
// 另在 ThreadPool 上跑“先等待、后完成”的 syncWait（走 parked 分支）。
//
// TSan 运行时自己定义了全局 operator new（与这里的替换版本链接冲突），所以 TSan 构建下
// 不替换、不计数，只跑行为检查；分配次数由其他配置验证。
#include <co2/co2.hpp>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define AW_ISSUE03_NO_COUNTING 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(AW_ISSUE03_NO_COUNTING)
#define AW_ISSUE03_NO_COUNTING 1
#endif

#include <cstdio>
#include <cstdlib>
#include <new>
#include <tuple>

namespace {
long allocations = 0;
int failures = 0;

#if defined(AW_ISSUE03_NO_COUNTING)
constexpr bool counting = false;
#else
constexpr bool counting = true;
#endif

void expectAllocations(char const* what, long actual, long expected);

void expectEqual(char const* what, long actual, long expected) {
    if (actual != expected) {
        std::printf("FAIL %s: %ld, expected %ld\n", what, actual, expected);
        ++failures;
    } else {
        std::printf("ok   %s: %ld\n", what, actual);
    }
}

void expectAllocations(char const* what, long actual, long expected) {
    if (counting) {
        expectEqual(what, actual, expected);
    } else {
        std::printf("skip %s (allocation counting disabled under TSan)\n", what);
    }
}

auto leaf(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto pairOf(int v) CO2_BEG(co2::Task<long>, (v), std::tuple<int, int> p{};) {
    CO2_AWAIT_SET(p, co2::whenAll(leaf(v), leaf(v)));
    CO2_RETURN(long{std::get<0>(p)} + std::get<1>(p));
}
CO2_END

auto onPool(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (pool, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_RETURN(v + 1);
}
CO2_END

template <class F> long allocationsPerOp(int ops, F&& op) {
    long const before = allocations;
    for (int i = 0; i != ops; ++i) op(i);
    return (allocations - before) / ops;
}
} // namespace

#if !defined(AW_ISSUE03_NO_COUNTING)
void* operator new(std::size_t n) {
    ++allocations;
    if (void* p = std::malloc(n != 0U ? n : 1U)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

int main() {
    long sum = 0;
    expectAllocations("syncWait(ready Task) allocations/op",
                allocationsPerOp(1000, [&](int i) { sum += co2::syncWait(leaf(i)); }), 1);
    expectAllocations("syncWait(Task{whenAll(2 ready)}) allocations/op",
                allocationsPerOp(1000, [&](int i) { sum += co2::syncWait(pairOf(i)); }), 5);
    co2::ManualExecutor executor;
    expectAllocations("spawn+run+join allocations/op", allocationsPerOp(1000, [&](int i) {
                    auto handle = co2::spawn(executor, leaf(i));
                    executor.run();
                    sum += handle.join();
                }),
                3);
    expectEqual("checksum", sum, 999L * 1000L / 2L * 4L);
    {
        co2::ThreadPool pool{2};
        long total = 0;
        for (int i = 0; i != 200; ++i) total += co2::syncWait(onPool(pool, i));
        expectEqual("syncWait on ThreadPool (wait before finish)", total, 199L * 200L / 2L + 200L);
    }
    return failures == 0 ? 0 : 1;
}
