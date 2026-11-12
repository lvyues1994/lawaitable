#pragma once

#include <climits>
#include <cstddef>
#include <new>

#include "awaitable/core/contract.hpp"

#ifndef AW_RECYCLING_ALLOCATOR_CACHE_SIZE
#define AW_RECYCLING_ALLOCATOR_CACHE_SIZE 2
#endif

#if defined(__GNUC__) || defined(__clang__)
#define AW_DETAIL_ALWAYS_INLINE inline __attribute__((always_inline))
#define AW_DETAIL_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define AW_DETAIL_ALWAYS_INLINE __forceinline
#define AW_DETAIL_NOINLINE __declspec(noinline)
#else
#define AW_DETAIL_ALWAYS_INLINE inline
#define AW_DETAIL_NOINLINE
#endif

namespace aw {
namespace detail {

// 照搬 asio thread_info_base：每个标签若干格缓存；以 4 字节为一块，分配时多要 1 字节，把
// 块数写在 mem[size]，归还时搬到 mem[0]；块数超过 255 的请求不进缓存。
enum class mem_tag : unsigned {
    default_tag = 0,
    awaitable_frame,
    executor_function,
    cancellation_signal,
    parallel_group
};
constexpr std::size_t mem_tag_count = 5U;

class thread_info {
  public:
    static constexpr std::size_t chunk_size = 4U;
    static constexpr std::size_t cache_size = AW_RECYCLING_ALLOCATOR_CACHE_SIZE;

    thread_info() noexcept {
        for (auto& row : reusable_)
            for (auto& cell : row) cell = nullptr;
    }
    thread_info(thread_info const&) = delete;
    thread_info& operator=(thread_info const&) = delete;
    ~thread_info() {
        for (auto& row : reusable_)
            for (auto& cell : row) ::operator delete(cell);
    }

    static void* allocate_block(std::size_t size) {
        std::size_t const chunks = (size + chunk_size - 1U) / chunk_size;
        void* const pointer = ::operator new(chunks * chunk_size + 1U);
        auto* const mem = static_cast<unsigned char*>(pointer);
        mem[size] = (chunks <= UCHAR_MAX) ? static_cast<unsigned char>(chunks) : 0U;
        return pointer;
    }

    // 快路径内联：命中缓存只做几次比较。对齐检查用掩码（align 是 2 的幂，见
    // recycling_allocate 的契约），不用取模——运行期的 % 会编译成除法指令。
    AW_DETAIL_ALWAYS_INLINE void* allocate(mem_tag tag, std::size_t size, std::size_t align) {
        std::size_t const chunks = (size + chunk_size - 1U) / chunk_size;
        auto& row = reusable_[static_cast<std::size_t>(tag)];
        for (auto& cell : row) {
            if (cell == nullptr) continue;
            auto* const mem = static_cast<unsigned char*>(cell);
            if (static_cast<std::size_t>(mem[0]) >= chunks &&
                (reinterpret_cast<std::size_t>(cell) & (align - 1U)) == 0U) {
                void* const pointer = cell;
                cell = nullptr;
                mem[size] = mem[0];
                return pointer;
            }
        }
        return allocate_miss(row, size);
    }

    // 没有合适的块：先放掉一块缓存，避免缓存里囤着不合用的块。
    AW_DETAIL_NOINLINE static void* allocate_miss(void* (&row)[cache_size], std::size_t size) {
        for (auto& cell : row) {
            if (cell != nullptr) {
                ::operator delete(cell);
                cell = nullptr;
                break;
            }
        }
        return allocate_block(size);
    }

    void deallocate(mem_tag tag, void* pointer, std::size_t size) noexcept {
        if (size <= chunk_size * UCHAR_MAX) {
            auto& row = reusable_[static_cast<std::size_t>(tag)];
            for (auto& cell : row) {
                if (cell == nullptr) {
                    auto* const mem = static_cast<unsigned char*>(pointer);
                    mem[0] = mem[size];
                    cell = pointer;
                    return;
                }
            }
        }
        ::operator delete(pointer);
    }

  private:
    void* reusable_[mem_tag_count][cache_size];
};

// 库线程的调用栈：thread_pool 的工作线程入口、manual_executor::run / run_one / poll 期间。
// 嵌套时共享最外层的 thread_info；最外层出栈时释放缓存。
class thread_context {
  public:
    thread_context() noexcept : next_{top()}, info_{next_ != nullptr ? next_->info_ : &own_} {
        top() = this;
        info_slot() = info_;
    }
    thread_context(thread_context const&) = delete;
    thread_context& operator=(thread_context const&) = delete;
    ~thread_context() {
        top() = next_;
        info_slot() = next_ != nullptr ? next_->info_ : nullptr;
    }

    // 分配快路径每次都要读：单独存一份线程局部指针，少一次间接和判空。
    static thread_info* current_info() noexcept { return info_slot(); }
    static bool in_library_thread() noexcept { return top() != nullptr; }

  private:
    static thread_context*& top() noexcept {
        static thread_local thread_context* current = nullptr;
        return current;
    }
    static thread_info*& info_slot() noexcept {
        static thread_local thread_info* info = nullptr;
        return info;
    }

    thread_context* next_;
    thread_info own_;
    thread_info* info_;
};

// ---- 线程局部标记 ----

// “启动中”：awaitable_thread::launch 的调用栈内，值为入口帧地址（§7.3 第 4 条）。
inline void const*& launching_marker() noexcept {
    static thread_local void const* marker = nullptr;
    return marker;
}
struct launching_scope {
    explicit launching_scope(void const* key) noexcept : previous_{launching_marker()} {
        launching_marker() = key;
    }
    launching_scope(launching_scope const&) = delete;
    launching_scope& operator=(launching_scope const&) = delete;
    ~launching_scope() { launching_marker() = previous_; }
    void const* previous_;
};

// “正在发起”：async_op_awaiter 调用 initiation 期间，值为 awaiter 地址（§8.3 同步完成）。
inline void const*& initiating_marker() noexcept {
    static thread_local void const* marker = nullptr;
    return marker;
}
struct initiating_scope {
    explicit initiating_scope(void const* key) noexcept : previous_{initiating_marker()} {
        initiating_marker() = key;
    }
    initiating_scope(initiating_scope const&) = delete;
    initiating_scope& operator=(initiating_scope const&) = delete;
    ~initiating_scope() { initiating_marker() = previous_; }
    void const* previous_;
};

// “正在展开被丢弃的执行线程”（§11）：此时销毁仍在帧栈里的生成器不算契约违规。
inline unsigned& unwinding_depth() noexcept {
    static thread_local unsigned depth = 0U;
    return depth;
}
inline bool unwinding() noexcept { return unwinding_depth() != 0U; }
struct unwinding_scope {
    unwinding_scope() noexcept { ++unwinding_depth(); }
    unwinding_scope(unwinding_scope const&) = delete;
    unwinding_scope& operator=(unwinding_scope const&) = delete;
    ~unwinding_scope() { --unwinding_depth(); }
};

} // namespace detail
} // namespace aw
