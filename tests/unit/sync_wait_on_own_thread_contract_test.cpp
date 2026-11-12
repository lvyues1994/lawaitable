// §10.3 / §14.4：在 ex 自己的线程上调 sync_wait(ex, …) 是契约违规（退出码 86）。
#include <cstdlib>

#include "awaitable/awaitable.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/sync_wait.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;

auto one() CO2_BEG((aw::awaitable<int, ex_t>), ()) { CO2_RETURN(1); }
CO2_END

struct nested_wait {
    ex_t ex;
    void operator()() const { static_cast<void>(aw::sync_wait(ex, one())); }
};

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    aw::manual_executor ctx;
    ctx.get_executor().post(nested_wait{ctx.get_executor()});
    ctx.run();
    return 0;
}
