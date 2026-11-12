#pragma once

#include <cstddef>
#include <exception>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/impl/awaitable_frame.hpp"
#include "awaitable/impl/awaitable_thread.hpp"

namespace aw {
namespace detail {

// ---- 完成参数 → await_resume 的结果（§7.1 第 4 条）----

template <class... Ts> struct op_lead { static constexpr std::size_t skip = 0U; };
template <class... Ts> struct op_lead<std::error_code, Ts...> { static constexpr std::size_t skip = 1U; };
template <class... Ts> struct op_lead<std::exception_ptr, Ts...> {
    static constexpr std::size_t skip = 1U;
};

template <class... Ts> struct op_value { using type = std::tuple<Ts...>; };
template <class T> struct op_value<T> { using type = T; };
template <> struct op_value<> { using type = void; };

template <std::size_t Skip, class... Ts> struct op_value_after;
template <class... Ts> struct op_value_after<0U, Ts...> { using type = typename op_value<Ts...>::type; };
template <class T, class... Ts> struct op_value_after<1U, T, Ts...> {
    using type = typename op_value<Ts...>::type;
};

inline void throw_if_error(std::error_code const& ec) {
    if (ec) throw std::system_error{ec};
}
inline void throw_if_error(std::exception_ptr const& e) {
    if (e) std::rethrow_exception(e);
}

template <class R> struct op_taker {
    template <std::size_t Skip, class Refs, std::size_t... I>
    static R take(Refs& refs, std::index_sequence<I...>) {
        return R(std::move(std::get<Skip + I>(refs))...);
    }
};
template <> struct op_taker<void> {
    template <std::size_t Skip, class Refs, std::size_t... I>
    static void take(Refs&, std::index_sequence<I...>) noexcept {}
};

template <class Signature> struct op_signature;
template <class... Ts> struct op_signature<void(Ts...)> {
    static_assert(std::is_same<std::tuple<Ts...>, std::tuple<typename std::decay<Ts>::type...>>::value,
                  "use_awaitable: completion signature arguments must be decayed object types");
    using refs_type = std::tuple<Ts&...>;
    static constexpr std::size_t skip = op_lead<Ts...>::skip;
    using result_type = typename op_value_after<skip, Ts...>::type;

    static result_type finish(refs_type& refs) {
        check(std::integral_constant<bool, skip == 1U>{}, refs);
        return op_taker<result_type>::template take<skip>(
            refs, std::make_index_sequence<sizeof...(Ts) - skip>{});
    }

  private:
    static void check(std::false_type, refs_type&) noexcept {}
    static void check(std::true_type, refs_type& refs) { throw_if_error(std::get<0>(refs)); }
};

// ---- 同步完成：参数移进投递的 operation，在那里 pump（§8.3 同步完成）----

template <class Executor, class... Ts> struct sync_resume_op {
    awaitable_thread<Executor> thread;
    std::tuple<Ts&...>** result;
    std::tuple<Ts...> values;

    void operator()() { run(std::index_sequence_for<Ts...>{}); }

    template <std::size_t... I> void run(std::index_sequence<I...>) {
        std::tuple<Ts&...> refs{std::get<I>(values)...};
        *result = &refs;
        thread.frame()->attach_thread(&thread);
        thread.frame()->clear_cancellation_slot();
        thread.pump();
    }
};

// §8.3：完成回调。移动持有执行线程；关联 executor / slot 来自执行线程（asio :804-1018）。
template <class Executor, class Signature> class awaitable_async_op_handler;

template <class Executor, class... Ts>
class awaitable_async_op_handler<Executor, void(Ts...)> : public awaitable_thread<Executor> {
  public:
    awaitable_async_op_handler(awaitable_thread<Executor>&& thread, std::tuple<Ts&...>** result,
                               void const* initiating_key) noexcept
        : awaitable_thread<Executor>{std::move(thread)}, result_{result}, key_{initiating_key} {}
    awaitable_async_op_handler(awaitable_async_op_handler&&) noexcept = default;

    void operator()(Ts... args) {
        if (initiating_marker() == key_) {
            // initiation 在自己的栈里同步调用了回调：不在这里 pump，先 post。
            Executor ex = this->get_executor();
            ex.post(sync_resume_op<Executor, Ts...>{awaitable_thread<Executor>{std::move(*this)},
                                                    result_, std::tuple<Ts...>{std::move(args)...}});
            return;
        }
        // 参数留在本栈上，awaiter 只拿到地址（asio :840-858）。
        std::tuple<Ts&...> refs{args...};
        *result_ = &refs;
        this->frame()->attach_thread(this);
        this->frame()->clear_cancellation_slot();
        this->pump();
    }

  private:
    std::tuple<Ts&...>** result_;
    void const* key_;
};

// §8.3：直接 awaiter，不为 use_awaitable 另开协程帧（asio :1020-1085）。
template <class Signature, class RequiredExecutor, class Initiation, class... Args> class async_op_awaiter;

template <class... Ts, class RequiredExecutor, class Initiation, class... Args>
class async_op_awaiter<void(Ts...), RequiredExecutor, Initiation, Args...> {
    using signature = op_signature<void(Ts...)>;

  public:
    using aw_awaitable_tag = void;

    template <class I, class... A>
    explicit async_op_awaiter(I&& init, A&&... args)
        : init_(std::forward<I>(init)), args_(std::forward<A>(args)...) {}
    async_op_awaiter(async_op_awaiter&&) = default;
    async_op_awaiter(async_op_awaiter const&) = delete;
    async_op_awaiter& operator=(async_op_awaiter const&) = delete;
    async_op_awaiter& operator=(async_op_awaiter&&) = delete;

    bool await_ready() const noexcept { return false; }

    template <class P> void await_suspend(co2::coroutine_handle<P> h) noexcept {
        using executor_type = typename P::executor_type;
        static_assert(std::is_void<RequiredExecutor>::value ||
                          std::is_same<RequiredExecutor, executor_type>::value,
                      "use_awaitable_t<E>: E must be the executor of the awaiting frame");
        awaitable_frame_base<executor_type>& frame = h.promise();
        frame_ = &frame;
        frame.after_suspend(&on_suspend<executor_type>, this);
    }

    typename signature::result_type await_resume() { return signature::finish(*result_); }

  private:
    // 帧的 resume() 返回之后执行：此时帧已挂起，可以把执行线程交给回调。
    template <class Executor> static void on_suspend(void* arg) {
        async_op_awaiter* const self = static_cast<async_op_awaiter*>(arg);
        auto* const frame = static_cast<awaitable_frame_base<Executor>*>(self->frame_);
        initiating_scope scope{self};
        awaitable_async_op_handler<Executor, void(Ts...)> handler{std::move(*frame->detach_thread()),
                                                                  &self->result_, self};
        // 回调可能在别的线程恢复帧并销毁本 awaiter，所以先把 init 和参数移到本栈上，
        // initiate 期间与之后都不再访问 self。
        Initiation init(std::move(self->init_));
        std::tuple<Args...> args(std::move(self->args_));
        initiate(init, std::move(handler), args, std::index_sequence_for<Args...>{});
    }

    template <class Handler, std::size_t... I>
    static void initiate(Initiation& init, Handler&& handler, std::tuple<Args...>& args,
                         std::index_sequence<I...>) {
        std::move(init)(std::forward<Handler>(handler), std::move(std::get<I>(args))...);
    }

    void* frame_{nullptr};
    std::tuple<Ts&...>* result_{nullptr};
    Initiation init_;
    std::tuple<Args...> args_;
};

} // namespace detail
} // namespace aw
