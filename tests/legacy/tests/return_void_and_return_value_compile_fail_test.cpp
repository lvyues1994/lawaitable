// 编译失败测试：promise 同时声明 return_void 与 return_value 时报错。
//
// 标准按名字查找判断，两者都找到声明即为病式（[dcl.fct.def.coroutine]/6）。修复前 co2
// 照常编过（co2::Task<void> 的 promise 自己就同时声明了两者，现已拆到按 T 特化的基类）。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#include "co2/coroutine.hpp"

struct EagerTask {
    struct promise_type {
        EagerTask get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
#ifdef CO2_EXPECT_COMPILE_FAILURE
        void return_value(int) noexcept {}
#endif
        void unhandled_exception() noexcept {}
    };
};

auto eager() CO2_BEG(EagerTask, ()) { CO2_RETURN(); }
CO2_END
