#pragma once

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/operation.hpp"

namespace aw {

// §5.2：保留 coro 的 Scheduler（改名），新增 post / running_in_this_thread。
class scheduler {
  public:
    scheduler() = default;
    scheduler(scheduler const&) = delete;
    scheduler& operator=(scheduler const&) = delete;
    virtual ~scheduler() = default;

    virtual void schedule(co2::coroutine_handle<> coroutine) noexcept = 0;
    virtual void post(detail::operation* op) noexcept = 0;
    virtual bool running_in_this_thread() const noexcept = 0;
};

// 原 ScheduleOn：只给 core 级协程用。awaitable 帧里等待它会被 await_transform 拒绝（§4.3）。
struct schedule_on_awaiter {
    scheduler* target;

    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<> coroutine) noexcept { target->schedule(coroutine); }
    void await_resume() const noexcept {}
};

inline schedule_on_awaiter schedule_on(scheduler& target) noexcept {
    return schedule_on_awaiter{&target};
}

} // namespace aw
