// 编译失败测试（§4.3 第 5 条、§14.4）：awaitable 帧里等待任意 awaiter 必须编译失败。
// 定义 AW_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。
#include "awaitable/awaitable.hpp"

using ex_t = aw::manual_executor::executor_type;

auto awaits_something() CO2_BEG((aw::awaitable<void, ex_t>), ()) {
#ifdef AW_EXPECT_COMPILE_FAILURE
    CO2_AWAIT(co2::suspend_always{});
#endif
    CO2_RETURN();
}
CO2_END
