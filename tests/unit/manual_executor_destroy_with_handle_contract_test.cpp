#include <cstdlib>
#include <string>
#include <vector>

#include "awaitable/exec/manual_executor.hpp"
#include "test_coro.hpp"

namespace {
auto hop(aw::scheduler& target, std::vector<std::string>& trace)
    CO2_BEG(aw_test::eager_coro, (target, trace)) {
    CO2_AWAIT(aw::schedule_on(target));
    trace.push_back("unreachable");
    CO2_RETURN();
}
CO2_END
} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    std::vector<std::string> trace;
    {
        aw::manual_executor ctx;
        hop(ctx, trace);
    } // 队列里还有协程句柄：契约违规
    return 0;
}
