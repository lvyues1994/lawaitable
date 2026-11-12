// 只包含总头文件：同时验证 <awaitable.hpp> 可用（§2.2）。
#include <awaitable.hpp>

#include <stdexcept>
#include <string>

#include "check.hpp"
#include "test_ops.hpp"

namespace {

using pool_ex = aw::thread_pool::executor_type;
using manual_ex = aw::manual_executor::executor_type;

auto ready(int v) CO2_BEG((aw::pool_awaitable<int>), (v)) { CO2_RETURN(v); }
CO2_END

auto later(aw_test::foreign_threads* threads, int v) CO2_BEG((aw::pool_awaitable<int>), (threads, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

auto fails() CO2_BEG((aw::pool_awaitable<void>), ()) {
    throw std::runtime_error{"sync"};
}
CO2_END

auto on_manual(manual_ex ex, int v) CO2_BEG((aw::manual_awaitable<int>), (ex, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_value(ex, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

auto manual_from_foreign(aw_test::foreign_threads* threads, int v)
    CO2_BEG((aw::manual_awaitable<int>), (threads, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

struct no_default {
    explicit no_default(int x) : v{x} {}
    int v;
};
auto make_no_default(int v) CO2_BEG((aw::pool_awaitable<no_default>), (v)) { CO2_RETURN(no_default{v}); }
CO2_END

void pool_finish_before_or_after_wait() {
    aw::thread_pool pool{2U};
    int total = 0;
    for (int i = 0; i != 200; ++i) total += aw::sync_wait(pool.get_executor(), ready(i)); // 多数先完成后等待
    AW_CHECK(total == 19900);
    aw_test::foreign_threads threads;
    AW_CHECK(aw::sync_wait(pool.get_executor(), later(&threads, 7)) == 7); // 先等待后完成
    AW_CHECK(aw::sync_wait(pool.get_executor(), make_no_default(5)).v == 5);
}

void pool_exception_is_rethrown() {
    aw::thread_pool pool{1U};
    try {
        aw::sync_wait(pool.get_executor(), fails());
        AW_CHECK(false);
    } catch (std::runtime_error const& e) {
        AW_CHECK(std::string{e.what()} == "sync");
    }
}

void manual_form_drives_run_one() {
    aw::manual_executor ctx;
    AW_CHECK(aw::sync_wait(ctx, on_manual(ctx.get_executor(), 11)) == 11);
    aw_test::foreign_threads threads;
    AW_CHECK(aw::sync_wait(ctx, manual_from_foreign(&threads, 12)) == 12);
}

} // namespace

int main() {
    pool_finish_before_or_after_wait();
    pool_exception_is_rethrown();
    manual_form_drives_run_one();
    return 0;
}
