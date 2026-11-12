// 编译失败测试：成员与非成员 operator co_await 同时可行时报错。
//
// 标准把两者放进同一次重载决议，此处二义（[over.match.oper]）。修复前 co2 悄悄选成员。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#include "co2/task.hpp"

namespace sample {

struct ReadyAwaiter {
    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    int await_resume() const noexcept { return value; }
    int value;
};

struct Wrapped {
    ReadyAwaiter operator_co_await() const noexcept { return ReadyAwaiter{1}; }
};

#ifdef CO2_EXPECT_COMPILE_FAILURE
inline ReadyAwaiter operator_co_await(Wrapped) noexcept { return ReadyAwaiter{2}; }
#endif

} // namespace sample

auto awaitWrapped() CO2_BEG(co2::Task<int>, (), int value{};) {
    CO2_AWAIT_SET(value, sample::Wrapped{});
    CO2_RETURN(value);
}
CO2_END
