#pragma once

// 支持取消的测试操作（§8.1 操作作者契约第 3 条）。

#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/core/detail/move_only_function.hpp"
#include "awaitable/error.hpp"
#include "test_ops.hpp"

namespace aw_test {

// void(std::error_code)：等到 set() 或被取消。只在 io_ex 上使用（单线程）。
template <class Executor> class manual_event {
  public:
    explicit manual_event(Executor const& io_ex) noexcept : io_ex_{io_ex} {}
    manual_event(manual_event const&) = delete;
    manual_event& operator=(manual_event const&) = delete;

    template <class Token> auto async_wait(Token&& token) {
        return aw::async_initiate<Token, void(std::error_code)>(initiate_wait{this}, token);
    }

    bool waiting() const noexcept { return static_cast<bool>(pending_); }
    aw::cancellation_type last_cancel() const noexcept { return last_cancel_; }

    // 成功完成：先清 slot（处理器指向本对象），再经关联 executor 交付。
    void set() {
        if (!pending_) return;
        slot_.clear();
        slot_ = aw::cancellation_slot{};
        auto pending = std::move(pending_);
        pending(std::error_code{});
    }

  private:
    struct initiate_wait {
        manual_event* self;
        template <class Handler> void operator()(Handler&& handler) const {
            self->start(std::forward<Handler>(handler));
        }
    };

    template <class Handler> struct completion {
        Handler handler;
        Executor io_ex;
        void operator()(std::error_code ec) {
            auto target = aw::get_associated_executor(handler, io_ex);
            target.dispatch(bind_completion(std::move(handler), ec));
        }
    };

    struct cancel_handler {
        manual_event* self;
        void operator()(aw::cancellation_type type) { self->cancel(type); }
    };

    template <class Handler> void start(Handler&& handler) {
        using handler_type = typename std::decay<Handler>::type;
        aw::cancellation_slot slot = aw::get_associated_cancellation_slot(handler);
        pending_ = co2::detail::MoveOnlyFunction<void(std::error_code)>(
            completion<handler_type>{std::forward<Handler>(handler), io_ex_});
        if (slot.is_connected()) {
            slot_ = slot;
            slot.template emplace<cancel_handler>(cancel_handler{this});
        }
    }

    // 在 slot 的 emit 里运行：不能清 slot（会销毁正在运行的处理器），交付总是 post。
    void cancel(aw::cancellation_type type) {
        last_cancel_ = type;
        if (!pending_) return;
        slot_ = aw::cancellation_slot{};
        auto pending = std::move(pending_);
        io_ex_.post(bind_completion(std::move(pending), std::error_code{aw::error::operation_aborted}));
    }

    Executor io_ex_;
    co2::detail::MoveOnlyFunction<void(std::error_code)> pending_;
    aw::cancellation_slot slot_;
    aw::cancellation_type last_cancel_{aw::cancellation_type::none};
};

// void(std::error_code, int)：在 slot 上装一个处理器，然后像 async_value 一样完成。
struct noop_cancel_handler {
    void operator()(aw::cancellation_type) noexcept {}
};

template <class Executor> struct initiate_cancellable_value {
    Executor io_ex;
    template <class Handler> void operator()(Handler&& handler, int v) const {
        aw::cancellation_slot slot = aw::get_associated_cancellation_slot(handler);
        if (slot.is_connected()) slot.template emplace<noop_cancel_handler>();
        initiate_complete_later<Executor>{io_ex}(std::forward<Handler>(handler), std::error_code{}, v);
    }
};

template <class Executor, class Token>
auto async_cancellable_value(Executor const& io_ex, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_cancellable_value<Executor>{io_ex},
                                                                 token, v);
}

} // namespace aw_test
