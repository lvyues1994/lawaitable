#pragma once

#include <cstddef>
#include <new>

#include "awaitable/core/contract.hpp"
#include "awaitable/exec/detail/thread_context.hpp"

namespace aw {
namespace detail {

// size 必须与分配时一致。只支持不超过 max_align_t 的对齐（C++14 没有对齐版 new）。
inline void* recycling_allocate(mem_tag tag, std::size_t size,
                                std::size_t align = alignof(std::max_align_t)) {
    CO2_CONTRACT_CHECK(align <= alignof(std::max_align_t));
#if defined(AW_DISABLE_RECYCLING_ALLOCATOR)
    static_cast<void>(tag);
    return ::operator new(size);
#else
    if (thread_info* const info = thread_context::current_info())
        return info->allocate(tag, size, align);
    return thread_info::allocate_block(size);
#endif
}

inline void recycling_deallocate(mem_tag tag, void* pointer, std::size_t size) noexcept {
#if defined(AW_DISABLE_RECYCLING_ALLOCATOR)
    static_cast<void>(tag);
    static_cast<void>(size);
    ::operator delete(pointer);
#else
    if (thread_info* const info = thread_context::current_info()) {
        info->deallocate(tag, pointer, size);
        return;
    }
    ::operator delete(pointer);
#endif
}

} // namespace detail
} // namespace aw
