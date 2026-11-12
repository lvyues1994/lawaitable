#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "check.hpp"
#include "counting_new.hpp"

using aw::detail::mem_tag;
using aw::detail::recycling_allocate;
using aw::detail::recycling_deallocate;

namespace {

void outsideLibraryThreadAlwaysAllocates() {
    AW_CHECK(!aw::detail::thread_context::in_library_thread());
    aw_test::allocation_counter counter;
    for (int i = 0; i != 3; ++i) {
        void* p = recycling_allocate(mem_tag::awaitable_frame, 100U);
        recycling_deallocate(mem_tag::awaitable_frame, p, 100U);
    }
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 3 && counter.deletes() == 3);
}

void reuseWithinTag() {
    aw::detail::thread_context context;
    AW_CHECK(aw::detail::thread_context::in_library_thread());
    void* first = recycling_allocate(mem_tag::awaitable_frame, 100U);
    recycling_deallocate(mem_tag::awaitable_frame, first, 100U);
    aw_test::allocation_counter counter;
    void* again = recycling_allocate(mem_tag::awaitable_frame, 100U);
    AW_CHECK(again == first);
    recycling_deallocate(mem_tag::awaitable_frame, again, 100U);
    void* smaller = recycling_allocate(mem_tag::awaitable_frame, 64U);  // 25 块够 16 块用
    AW_CHECK(smaller == first);
    recycling_deallocate(mem_tag::awaitable_frame, smaller, 64U);
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 0);
}

void tagsAreSeparate() {
    aw::detail::thread_context context;
    void* p = recycling_allocate(mem_tag::executor_function, 48U);
    recycling_deallocate(mem_tag::executor_function, p, 48U);
    aw_test::allocation_counter counter;
    void* q = recycling_allocate(mem_tag::cancellation_signal, 48U);
    recycling_deallocate(mem_tag::cancellation_signal, q, 48U);
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 1);
}

void cacheHoldsTwoBlocksPerTag() {
    aw::detail::thread_context context;
    void* a = recycling_allocate(mem_tag::default_tag, 32U);
    void* b = recycling_allocate(mem_tag::default_tag, 32U);
    void* c = recycling_allocate(mem_tag::default_tag, 32U);
    aw_test::allocation_counter counter;
    recycling_deallocate(mem_tag::default_tag, a, 32U);
    recycling_deallocate(mem_tag::default_tag, b, 32U);
    recycling_deallocate(mem_tag::default_tag, c, 32U);  // 第三块放不下，直接 delete
    if (aw_test::counting_new_enabled) AW_CHECK(counter.deletes() == 1);
}

void hugeRequestsBypassTheCache() {
    aw::detail::thread_context context;
    aw_test::allocation_counter counter;
    for (int i = 0; i != 2; ++i) {
        void* p = recycling_allocate(mem_tag::awaitable_frame, 4U * 300U);  // 300 块 > 255
        recycling_deallocate(mem_tag::awaitable_frame, p, 4U * 300U);
    }
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 2 && counter.deletes() == 2);
}

void nestedContextsShareTheOuterCache() {
    aw::detail::thread_context outer;
    void* p = nullptr;
    {
        aw::detail::thread_context inner;
        p = recycling_allocate(mem_tag::awaitable_frame, 80U);
        recycling_deallocate(mem_tag::awaitable_frame, p, 80U);
    }
    aw_test::allocation_counter counter;
    void* q = recycling_allocate(mem_tag::awaitable_frame, 80U);
    AW_CHECK(q == p);
    recycling_deallocate(mem_tag::awaitable_frame, q, 80U);
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 0);
}

void contextExitFreesCachedBlocks() {
    aw_test::allocation_counter counter;
    {
        aw::detail::thread_context context;
        void* p = recycling_allocate(mem_tag::awaitable_frame, 80U);
        recycling_deallocate(mem_tag::awaitable_frame, p, 80U);
    }
    AW_CHECK(!aw::detail::thread_context::in_library_thread());
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 1 && counter.deletes() == 1);
}

void blockFreedOnOtherThreadKindStillValid() {
    void* p = recycling_allocate(mem_tag::awaitable_frame, 40U);  // 非库线程分配
    aw::detail::thread_context context;
    recycling_deallocate(mem_tag::awaitable_frame, p, 40U);       // 库线程释放：进缓存
    void* q = recycling_allocate(mem_tag::awaitable_frame, 40U);
    AW_CHECK(q == p);
    recycling_deallocate(mem_tag::awaitable_frame, q, 40U);
}

void markers() {
    int key = 0;
    AW_CHECK(aw::detail::launching_marker() == nullptr);
    {
        aw::detail::launching_scope outer{&key};
        AW_CHECK(aw::detail::launching_marker() == &key);
        {
            aw::detail::launching_scope inner{nullptr};
            AW_CHECK(aw::detail::launching_marker() == nullptr);
        }
        AW_CHECK(aw::detail::launching_marker() == &key);
    }
    {
        aw::detail::initiating_scope scope{&key};
        AW_CHECK(aw::detail::initiating_marker() == &key);
    }
    AW_CHECK(aw::detail::initiating_marker() == nullptr);
    AW_CHECK(!aw::detail::unwinding());
    {
        aw::detail::unwinding_scope scope;
        AW_CHECK(aw::detail::unwinding());
    }
    AW_CHECK(!aw::detail::unwinding());
}

} // namespace

int main() {
    outsideLibraryThreadAlwaysAllocates();
    reuseWithinTag();
    tagsAreSeparate();
    cacheHoldsTwoBlocksPerTag();
    hugeRequestsBypassTheCache();
    nestedContextsShareTheOuterCache();
    contextExitFreesCachedBlocks();
    blockFreedOnOtherThreadKindStillValid();
    markers();
    return 0;
}
