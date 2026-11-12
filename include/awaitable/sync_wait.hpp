#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/exec/executor.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/impl/entry_point.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

// §10.3：补丁 03 的三态协议。完成方只有看到 parked 才加锁；等待方先快速 load，
// 再在锁内用 CAS 把 running 改成 parked。
class sync_wait_event {
  public:
    void set() noexcept {
        if (phase_.exchange(finished, std::memory_order_acq_rel) == parked) {
            std::lock_guard<std::mutex> lock{mutex_};
            notified_ = true;
            cv_.notify_one();
        }
    }

    bool is_set() const noexcept { return phase_.load(std::memory_order_acquire) == finished; }

    void wait() {
        if (is_set()) return;
        std::unique_lock<std::mutex> lock{mutex_};
        unsigned char expected = running;
        if (!phase_.compare_exchange_strong(expected, parked, std::memory_order_acq_rel)) return;
        cv_.wait(lock, [this] { return notified_; });
    }

  private:
    static constexpr unsigned char running = 0U;
    static constexpr unsigned char finished = 1U;
    static constexpr unsigned char parked = 2U;

    std::atomic<unsigned char> phase_{running};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool notified_{false};
};

template <class T> struct sync_wait_state {
    sync_wait_event event;
    std::exception_ptr error{};
    aw::optional<spawn_value_t<T>> value{};

    T take() {
        if (error) std::rethrow_exception(error);
        return unwrap(*value);
    }

  private:
    template <class V> static T unwrap(V& v) { return std::move(v); }
    template <class V> static T unwrap(aw::optional<V>& v) { return std::move(*v); }
};
template <> struct sync_wait_state<void> {
    sync_wait_event event;
    std::exception_ptr error{};
    void take() {
        if (error) std::rethrow_exception(error);
    }
};

template <class T> struct sync_wait_handler {
    sync_wait_state<T>* state;
    template <class V> void operator()(std::exception_ptr e, V&& v) {
        state->error = std::move(e);
        if (!state->error) state->value.emplace(std::forward<V>(v));
        state->event.set(); // 之后不再访问 state
    }
};
template <> struct sync_wait_handler<void> {
    sync_wait_state<void>* state;
    void operator()(std::exception_ptr e) {
        state->error = std::move(e);
        state->event.set();
    }
};

} // namespace detail

// 形式一：co_spawn 到 ex，阻塞调用线程直到交付。前置条件：!ex.running_in_this_thread()。
template <class T, class Executor, typename std::enable_if<is_executor<Executor>::value, int>::type = 0>
T sync_wait(Executor const& ex, awaitable<T, Executor> a) {
    // 在 ex 自己的线程上等待会死锁（§14.4 契约违规）。
    CO2_CONTRACT_CHECK(!ex.running_in_this_thread());
    detail::sync_wait_state<T> state;
    co_spawn(ex, std::move(a), detail::sync_wait_handler<T>{&state});
    state.event.wait();
    return state.take();
}

// 形式二：在调用线程上驱动 ctx.run_one()，直到交付完成。外部线程完成的操作会投递回 ctx。
template <class T> T sync_wait(manual_executor& ctx, awaitable<T, manual_executor::executor_type> a) {
    CO2_CONTRACT_CHECK(!ctx.running_in_this_thread());
    detail::sync_wait_state<T> state;
    co_spawn(ctx.get_executor(), std::move(a), detail::sync_wait_handler<T>{&state});
    while (!state.event.is_set()) {
        if (!ctx.run_one()) std::this_thread::yield();
    }
    return state.take();
}

} // namespace aw
