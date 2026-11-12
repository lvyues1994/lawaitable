#define AW_DISABLE_RECYCLING_ALLOCATOR 1
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "check.hpp"
#include "counting_new.hpp"

int main() {
    aw::detail::thread_context context;
    aw_test::allocation_counter counter;
    for (int i = 0; i != 4; ++i) {
        void* p = aw::detail::recycling_allocate(aw::detail::mem_tag::awaitable_frame, 100U);
        aw::detail::recycling_deallocate(aw::detail::mem_tag::awaitable_frame, p, 100U);
    }
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 4 && counter.deletes() == 4);
    return 0;
}
