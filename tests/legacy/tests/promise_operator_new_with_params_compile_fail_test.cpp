// 编译失败测试：promise 只提供 operator new(size_t, 参数...) 时报错。
//
// 标准先尝试 operator new(size, 参数左值...)（[dcl.fct.def.coroutine]/9），co2 先分配帧再把
// 参数移进去，无法传参（设计稿 D9）。修复前 co2 会悄悄改用 allocator，promise 的
// operator new 一次也不被调用；现在是编译错误。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#include <cstddef>
#include <cstdlib>
#include <new>

#include "co2/coroutine.hpp"

struct AllocatingTask {
    struct promise_type {
#ifdef CO2_EXPECT_COMPILE_FAILURE
        static void* operator new(std::size_t size, int) {
            void* const memory = std::malloc(size);
            if (memory == nullptr) throw std::bad_alloc{};
            return memory;
        }
#else
        static void* operator new(std::size_t size) {
            void* const memory = std::malloc(size);
            if (memory == nullptr) throw std::bad_alloc{};
            return memory;
        }
#endif
        static void operator delete(void* const memory) noexcept { std::free(memory); }

        AllocatingTask get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
};

auto allocating(int value) CO2_BEG(AllocatingTask, (value)) {
    static_cast<void>(value);
    CO2_RETURN();
}
CO2_END
