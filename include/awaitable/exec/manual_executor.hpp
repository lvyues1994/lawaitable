#pragma once

#include <cstddef>
#include <mutex>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/detail/ring_queue.hpp"
#include "awaitable/exec/detail/operation.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/exec/executor.hpp"
#include "awaitable/exec/scheduler.hpp"

namespace aw {

// §5.5：调用方驱动的 FIFO 执行上下文。run / run_one / poll 期间调用线程是库线程。
class manual_executor final : public scheduler {
  public:
    using executor_type = basic_executor<manual_executor>;

    manual_executor() = default;
    ~manual_executor() override { shutdown(); }

    executor_type get_executor() noexcept { return executor_type{*this}; }

    void schedule(co2::coroutine_handle<> coroutine) noexcept override {
        CO2_CONTRACT_CHECK(coroutine);
        std::lock_guard<std::mutex> lock{mutex_};
        CO2_CONTRACT_CHECK(!shutting_down_);
        ready_.push(detail::tag_handle(coroutine));
    }

    void post(detail::operation* op) noexcept override {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (!shutting_down_) {
                ready_.push(detail::tag_operation(op));
                return;
            }
        }
        op->destroy(); // 关闭期间投递的项立即销毁（§5.4 第 5 条）
    }

    bool running_in_this_thread() const noexcept override { return driver() == this; }

    bool run_one() {
        driver_scope scope{this};
        return run_one_in_scope();
    }

    std::size_t run() {
        driver_scope scope{this};
        std::size_t count = 0U;
        while (run_one_in_scope()) ++count;
        return count;
    }

    // 只执行调用时已经就绪的项。
    std::size_t poll() {
        driver_scope scope{this};
        std::size_t ready_now = pending();
        std::size_t count = 0U;
        while (ready_now-- != 0U && run_one_in_scope()) ++count;
        return count;
    }

    std::size_t pending() const noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        return ready_.size();
    }

  private:
    struct driver_scope {
        explicit driver_scope(manual_executor* self) noexcept : previous{driver()} {
            driver() = self;
        }
        driver_scope(driver_scope const&) = delete;
        driver_scope& operator=(driver_scope const&) = delete;
        ~driver_scope() { driver() = previous; }
        manual_executor* previous;
        detail::thread_context context;
    };

    static manual_executor*& driver() noexcept {
        static thread_local manual_executor* current = nullptr;
        return current;
    }

    bool run_one_in_scope() {
        void* item = nullptr;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (ready_.empty()) return false;
            item = ready_.pop();
        }
        detail::run_item(item);
        return true;
    }

    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            shutting_down_ = true;
        }
        for (;;) {
            void* item = nullptr;
            {
                std::lock_guard<std::mutex> lock{mutex_};
                if (ready_.empty()) return;
                item = ready_.pop();
            }
            detail::destroy_item(item);
        }
    }

    mutable std::mutex mutex_;
    co2::detail::RingQueue<void*> ready_;
    bool shutting_down_{false};
};

} // namespace aw
