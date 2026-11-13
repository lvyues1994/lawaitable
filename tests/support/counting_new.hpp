#pragma once

// 替换全局 operator new / delete 并计数。每个测试可执行文件只允许一个翻译单元包含本文件。
// TSan 运行时自己定义了全局 operator new（与 Task 1 的 issue03 测试同一原因），TSan 构建下
// 不替换、不计数：counting_new_enabled 为 false，测试只做行为检查。

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define AW_TEST_NO_COUNTING_NEW 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define AW_TEST_NO_COUNTING_NEW 1
#endif

namespace aw_test {

inline std::atomic<long>& new_calls() noexcept {
    static std::atomic<long> calls{0};
    return calls;
}

inline std::atomic<long>& delete_calls() noexcept {
    static std::atomic<long> calls{0};
    return calls;
}

#if defined(AW_TEST_NO_COUNTING_NEW)
constexpr bool counting_new_enabled = false;
#else
constexpr bool counting_new_enabled = true;
#endif

class allocation_counter {
  public:
    allocation_counter() noexcept
        : news_at_start_{new_calls().load()}, deletes_at_start_{delete_calls().load()} {}
    long news() const noexcept { return new_calls().load() - news_at_start_; }
    long deletes() const noexcept { return delete_calls().load() - deletes_at_start_; }

  private:
    long news_at_start_;
    long deletes_at_start_;
};

} // namespace aw_test

#if !defined(AW_TEST_NO_COUNTING_NEW)
// GCC 14 -O2 把替换的 operator new / delete 内联成 malloc / free 后，会把配对的调用误报成
// -Wmismatched-new-delete；不让它们内联即可（计数测试不关心这里的速度）。
#if defined(__GNUC__) && !defined(__clang__)
#define AW_TEST_COUNTING_NEW_NOINLINE __attribute__((noinline))
#else
#define AW_TEST_COUNTING_NEW_NOINLINE
#endif
AW_TEST_COUNTING_NEW_NOINLINE void* operator new(std::size_t size) {
    aw_test::new_calls().fetch_add(1, std::memory_order_relaxed);
    if (void* const p = std::malloc(size == 0U ? 1U : size)) return p;
    throw std::bad_alloc{};
}
AW_TEST_COUNTING_NEW_NOINLINE void operator delete(void* p) noexcept {
    if (p != nullptr) aw_test::delete_calls().fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
AW_TEST_COUNTING_NEW_NOINLINE void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
