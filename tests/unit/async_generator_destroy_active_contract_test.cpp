// §10.2 第 6 条：销毁正在帧栈中（挂起在异步操作上）的生成器是契约违规（退出码 86）。
#include <cstdlib>

#include "awaitable/async_generator.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/use_awaitable.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
using gen_t = aw::async_generator<int, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

auto slow(event_t* ev) CO2_BEG((gen_t), (ev)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_YIELD(1);
    CO2_RETURN();
}
CO2_END

auto pull(gen_t* g) CO2_BEG((aw::awaitable<void, ex_t>), (g), aw::optional<int> v{};) {
    CO2_AWAIT_SET(v, g->next());
    CO2_RETURN();
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::optional<gen_t> holder{slow(&ev)};
    aw::co_spawn(ctx, pull(&*holder), aw::detached);
    ctx.run();
    holder.reset();
    return 0;
}
