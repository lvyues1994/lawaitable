// 规格增补 C3 第 1 条：drop_* 策略配容量 0 是契约违规（退出码 86）。
#include <cstdlib>

#include "awaitable/channel.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/exec/manual_executor.hpp"

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    aw::manual_executor ctx;
    aw::channel<int, aw::manual_executor::executor_type> ch{ctx, 0, aw::channel_policy::drop_oldest};
    static_cast<void>(ch);
    return 0;
}
