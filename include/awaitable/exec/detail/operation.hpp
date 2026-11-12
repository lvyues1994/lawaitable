#pragma once

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"

namespace aw {
namespace detail {

// §5.3：队列里的普通函数。invoke=true 执行并释放；invoke=false 只销毁（关闭阶段）。
struct operation {
    using complete_fn = void (*)(operation*, bool invoke);

    explicit operation(complete_fn fn) noexcept : complete_{fn} {}

    void complete() { complete_(this, true); }
    void destroy() noexcept {
        // invoke=false 的路径只做析构和释放，不会抛出。
        complete_(this, false);
    }

    complete_fn complete_;
    operation* next_{nullptr}; // strand 的侵入式队列
};

template <class F> struct function_operation final : operation {
    template <class G>
    explicit function_operation(G&& g) : operation{&do_complete}, function_(std::forward<G>(g)) {}

    static void do_complete(operation* base, bool invoke) {
        auto* const self = static_cast<function_operation*>(base);
        // 先把函数移到栈上并归还内存，再执行：执行期间投递的下一个 operation 能复用这块内存。
        F function(std::move(self->function_));
        self->~function_operation();
        recycling_deallocate(mem_tag::executor_function, self, sizeof(function_operation));
        if (invoke) function();
    }

    F function_;
};

template <class F> operation* make_function_operation(F&& f) {
    using op_type = function_operation<typename std::decay<F>::type>;
    void* const memory =
        recycling_allocate(mem_tag::executor_function, sizeof(op_type), alignof(op_type));
    try {
        return ::new (memory) op_type(std::forward<F>(f));
    } catch (...) {
        recycling_deallocate(mem_tag::executor_function, memory, sizeof(op_type));
        throw;
    }
}

// ---- 带标记的队列项：最低位 0 = 协程帧地址，1 = operation* ----

inline void* tag_handle(co2::coroutine_handle<> handle) noexcept { return handle.address(); }

inline void* tag_operation(operation* op) noexcept {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(op) | std::uintptr_t{1});
}

inline bool is_operation(void* item) noexcept {
    return (reinterpret_cast<std::uintptr_t>(item) & std::uintptr_t{1}) != 0U;
}

inline operation* as_operation(void* item) noexcept {
    return reinterpret_cast<operation*>(reinterpret_cast<std::uintptr_t>(item) &
                                        ~std::uintptr_t{1});
}

inline void run_item(void* item) {
    if (is_operation(item)) {
        as_operation(item)->complete();
    } else {
        co2::coroutine_handle<>::from_address(item).resume();
    }
}

inline void destroy_item(void* item) noexcept {
    if (is_operation(item)) {
        as_operation(item)->destroy();
    } else {
        CO2_CONTRACT_FAIL("an execution context was destroyed with a queued coroutine handle");
    }
}

} // namespace detail
} // namespace aw
