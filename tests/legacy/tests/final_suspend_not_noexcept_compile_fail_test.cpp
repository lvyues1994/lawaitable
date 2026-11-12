// 编译失败测试：co_await promise.final_suspend() 可能抛出时报错。
//
// 标准要求整个表达式不得潜在抛出（[dcl.fct.def.coroutine]/15）。co2 的 final suspend 在
// noexcept 函数里执行，修复前非 noexcept 的 final_suspend 能编过，一旦抛出就 terminate。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#include "co2/coroutine.hpp"

#ifdef CO2_EXPECT_COMPILE_FAILURE
#define CO2_TEST_FINAL_NOEXCEPT
#else
#define CO2_TEST_FINAL_NOEXCEPT noexcept
#endif

struct EagerTask {
    struct promise_type {
        EagerTask get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() CO2_TEST_FINAL_NOEXCEPT { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
};

auto eager() CO2_BEG(EagerTask, ()) { CO2_RETURN(); }
CO2_END
