#pragma once

#include <cstddef>
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/error.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/optional.hpp"
#include "awaitable/this_coro.hpp"

namespace aw {

template <class T, class Executor> class awaitable;

namespace detail {

template <class Executor> class awaitable_thread;
template <class Executor> class entry_frame_base;
struct recycled_entry_storage;
template <class Executor, class T, class Handler, class Storage = recycled_entry_storage>
class entry_frame_impl;

// 帧 resume() 在栈上放一个，after_suspend 写进去（asio impl/awaitable.hpp:497-516）。
struct resume_context {
    void (*after_suspend_fn)(void*){nullptr};
    void* after_suspend_arg{nullptr};
};

template <class T> struct dependent_false : std::false_type {};
template <class...> struct make_void { using type = void; };

template <class T, class = void> struct has_aw_awaitable_tag : std::false_type {};
template <class T>
struct has_aw_awaitable_tag<T, typename make_void<typename T::aw_awaitable_tag>::type>
    : std::true_type {};

template <class T, class = void> struct has_aw_this_coro_tag : std::false_type {};
template <class T>
struct has_aw_this_coro_tag<T, typename make_void<typename T::aw_this_coro_tag>::type>
    : std::true_type {};

template <class T, class Executor> struct is_awaitable_of : std::false_type {};
template <class U, class Executor>
struct is_awaitable_of<awaitable<U, Executor>, Executor> : std::true_type {};

// 交出 exception_ptr，原处不留引用。老版本 libc++（LLVM 20 及以前、NDK r25c、QNX SDP 7.1）的
// exception_ptr 没有移动构造 / 移动赋值，std::move 会退化成复制，原对象仍持有引用；这里复制后显式置空。
// 返回的纯右值直接初始化接收方（形参 / 成员），不会再多出副本。没有异常是常态：先做内联的判空，
// 免得在 libc++ 上为空指针多调两次不内联的复制 / 赋值（co_spawn 每次交付都会经过这里）。
inline std::exception_ptr take_exception(std::exception_ptr& e) noexcept {
    if (!e) return std::exception_ptr{};
    std::exception_ptr out = std::move(e);
    e = nullptr;
    return out;
}

// 投递用的异常槽：移动时源对象一定置空（见 take_exception）。装进 operation 的结构体移动时，
// 被移走的那份不会在投递线程上继续持有异常引用。
class exception_handoff {
  public:
    exception_handoff() = default;
    explicit exception_handoff(std::exception_ptr e) noexcept : e_(take_exception(e)) {}
    exception_handoff(exception_handoff&& other) noexcept : e_(take_exception(other.e_)) {}
    exception_handoff& operator=(exception_handoff&& other) noexcept {
        e_ = take_exception(other.e_);
        return *this;
    }
    std::exception_ptr take() noexcept { return take_exception(e_); }

  private:
    std::exception_ptr e_;
};

// 帧里待重抛的异常。std::exception_ptr 的析构在 libc++ / libstdc++ 里是不内联的库调用，
// 即使为空也要调一次；绝大多数帧没有异常，所以放进 union，只在非空时才析构（判空是内联
// 的指针比较）。大小与 std::exception_ptr 相同，帧布局不变（§9.1）。
class pending_exception {
  public:
    pending_exception() noexcept { ::new (static_cast<void*>(&value_)) std::exception_ptr(); }
    pending_exception(pending_exception const&) = delete;
    pending_exception& operator=(pending_exception const&) = delete;
    ~pending_exception() {
        if (value_) value_.~exception_ptr();
    }

    explicit operator bool() const noexcept { return static_cast<bool>(value_); }

    void store(std::exception_ptr e) noexcept {
        if (e) value_ = std::move(e);
    }

    std::exception_ptr take() noexcept {
        if (!value_) return std::exception_ptr{};
        std::exception_ptr e = std::move(value_);
        value_ = nullptr;
        return e;
    }

  private:
    union {
        std::exception_ptr value_;
    };
};

template <class Executor> class awaitable_frame_base {
  public:
    using executor_type = Executor;

    awaitable_frame_base() noexcept = default;
    awaitable_frame_base(awaitable_frame_base const&) = delete;
    awaitable_frame_base& operator=(awaitable_frame_base const&) = delete;

    // §9.4：core 的 FrameMemory 优先用 promise 的 operator new(size_t)。
    static void* operator new(std::size_t size) {
        return recycling_allocate(mem_tag::awaitable_frame, size);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        recycling_deallocate(mem_tag::awaitable_frame, pointer, size);
    }

    co2::suspend_always initial_suspend() const noexcept { return {}; }

    struct final_awaiter {
        awaitable_frame_base* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept { self->pop_frame(); }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() noexcept { return final_awaiter{this}; }

    void unhandled_exception() noexcept { pending_exception_.store(std::current_exception()); }

    void rethrow_exception() {
        if (pending_exception_) rethrow_pending();
    }
    AW_DETAIL_NOINLINE void rethrow_pending() { std::rethrow_exception(pending_exception_.take()); }

    // asio impl/awaitable.hpp:480-495。
    void push_frame(awaitable_frame_base* caller) noexcept {
        caller_ = caller;
        attached_thread_ = caller_->attached_thread_;
        attached_thread_->entry_point()->top_of_stack_ = this;
        caller_->attached_thread_ = nullptr;
    }

    void pop_frame() noexcept {
        if (caller_ != nullptr) caller_->attached_thread_ = attached_thread_;
        attached_thread_->entry_point()->top_of_stack_ = caller_;
        attached_thread_ = nullptr;
        caller_ = nullptr;
    }

    void attach_thread(awaitable_thread<Executor>* thread) noexcept { attached_thread_ = thread; }

    awaitable_thread<Executor>* detach_thread() noexcept {
        awaitable_thread<Executor>* const thread = attached_thread_;
        attached_thread_ = nullptr;
        return thread;
    }

    void resume() {
        resume_context context;
        resume_context_ = &context;
        coro_.resume();
        // 帧可能已在别的线程上继续运行：此后只读栈上的 context。
        if (context.after_suspend_fn != nullptr) context.after_suspend_fn(context.after_suspend_arg);
    }

    void after_suspend(void (*fn)(void*), void* arg) noexcept {
        resume_context_->after_suspend_fn = fn;
        resume_context_->after_suspend_arg = arg;
    }

    entry_frame_base<Executor>* entry() const noexcept { return attached_thread_->entry_point(); }

    void clear_cancellation_slot() { entry()->cancellation_state_.slot().clear(); }

    void destroy() noexcept { coro_.destroy(); }

    // ---- §4.3 await_transform ----

    template <class U>
    awaitable<U, Executor> await_transform(awaitable<U, Executor>&& child) const {
        throw_if_cancelled_now();
        return std::move(child);
    }

    template <class Op, typename std::enable_if<
                            has_aw_awaitable_tag<typename std::decay<Op>::type>::value, int>::type = 0>
    Op&& await_transform(Op&& op) const {
        throw_if_cancelled_now();
        return std::forward<Op>(op);
    }

    struct executor_awaiter {
        Executor ex;
        bool await_ready() const noexcept { return true; }
        void await_suspend(co2::coroutine_handle<>) const noexcept {}
        Executor await_resume() const noexcept { return ex; }
    };
    executor_awaiter await_transform(this_coro::executor_t) const noexcept {
        return executor_awaiter{entry()->executor_};
    }

    template <class Other,
              typename std::enable_if<
                  !is_awaitable_of<typename std::decay<Other>::type, Executor>::value &&
                      !has_aw_awaitable_tag<typename std::decay<Other>::type>::value &&
                      !has_aw_this_coro_tag<typename std::decay<Other>::type>::value,
                  int>::type = 0>
    co2::suspend_never await_transform(Other&&) const {
        static_assert(dependent_false<Other>::value,
                      "an aw::awaitable frame can only await aw::awaitable (same executor), "
                      "use_awaitable operations, this_coro queries and async_generator::next()");
        return {};
    }

    // ---- §4.5 / §6.2 / §6.4：取消相关的 this_coro 查询（Task 13）----

    template <class V> struct ready_value {
        V value;
        bool await_ready() const noexcept { return true; }
        void await_suspend(co2::coroutine_handle<>) const noexcept {}
        V await_resume() { return std::move(value); }
    };

    ready_value<cancellation_state> await_transform(this_coro::cancellation_state_t) const noexcept {
        return ready_value<cancellation_state>{entry()->cancellation_state_};
    }

    template <class In, class Out>
    co2::suspend_never await_transform(this_coro::reset_cancellation_state_t<In, Out> reset) const {
        entry()->reset_cancellation_state(reset.in, reset.out);
        return {};
    }

    ready_value<bool> await_transform(this_coro::throw_if_cancelled_0_t) const noexcept {
        return ready_value<bool>{entry()->throw_if_cancelled_};
    }

    co2::suspend_never await_transform(this_coro::throw_if_cancelled_1_t set) const noexcept {
        entry()->throw_if_cancelled_ = set.value;
        return {};
    }

    ready_value<stop_token> await_transform(get_stop_token_t) const {
        return ready_value<stop_token>{entry()->bridge().token()};
    }

    // coro 旧写法：inherit_stop_token(into, handle) 借用这里的 EnvStopToken（§6.4）。
    detail::EnvStopToken const& envStopToken() const { return entry()->bridge().env(); }

    void throw_if_cancelled_now() const {
        entry_frame_base<Executor>* const e = entry();
        if (e->throw_if_cancelled_ && !!e->cancellation_state_.cancelled()) throw_cancelled();
    }
    AW_DETAIL_NOINLINE static void throw_cancelled() {
        // 直接调 make_error_code，不走 std::error_code 的枚举构造函数：GCC 8 + libc++（QNX 7.1）
        // 会把那个构造函数的默认实参 `= 0` 在这里报成 -Wzero-as-null-pointer-constant。
        throw std::system_error{error::make_error_code(error::operation_aborted), "co_await"};
    }

    // ---- 数据成员（asio :523-528）----
    co2::coroutine_handle<> coro_{};
    awaitable_thread<Executor>* attached_thread_{nullptr};
    awaitable_frame_base* caller_{nullptr};
    pending_exception pending_exception_;
    resume_context* resume_context_{nullptr};
};

template <class T> struct frame_result {
    aw::optional<T> value;
    template <class U> void set(U&& u) { value.emplace(std::forward<U>(u)); }
    T take() { return std::move(*value); }
};
template <class T> struct frame_result<T&> {
    T* value{nullptr};
    void set(T& u) noexcept { value = &u; }
    T& take() noexcept { return *value; }
};

template <class T, class Executor> class awaitable_frame : public awaitable_frame_base<Executor> {
  public:
    awaitable<T, Executor> get_return_object() noexcept;

    template <class U> void return_value(U&& v) { result_.set(std::forward<U>(v)); }

    T get() {
        this->caller_ = nullptr;
        this->rethrow_exception();
        return result_.take();
    }

  private:
    frame_result<T> result_;
};

template <class Executor>
class awaitable_frame<void, Executor> : public awaitable_frame_base<Executor> {
  public:
    awaitable<void, Executor> get_return_object() noexcept;

    void return_void() noexcept {}

    void get() {
        this->caller_ = nullptr;
        this->rethrow_exception();
    }
};

} // namespace detail
} // namespace aw
