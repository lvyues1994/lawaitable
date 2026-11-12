#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/dispatch.hpp"
#include "awaitable/error.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/post.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;

// 示例操作：经回调的关联 executor 交付 (error_code, int)。
struct initiate_add {
    ex_t ex;
    template <class Handler> void operator()(Handler&& handler, int a, int b, bool fail) const {
        auto target = aw::get_associated_executor(handler, ex);
        ex.post([handler = std::forward<Handler>(handler), target, a, b, fail]() mutable {
            std::error_code const ec =
                fail ? std::error_code{aw::error::operation_aborted} : std::error_code{};
            target.dispatch([handler = std::move(handler), ec, a, b]() mutable { handler(ec, a + b); });
        });
    }
};

template <class Token> auto async_add(ex_t ex, int a, int b, bool fail, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_add{ex}, token, a, b, fail);
}

// 同步调用回调的 initiation，用来测 use_future 的各种签名。
template <class Signature> struct initiate_now;
template <class... Ts> struct initiate_now<void(Ts...)> {
    template <class Handler, class... Args> void operator()(Handler&& handler, Args&&... args) const {
        handler(std::forward<Args>(args)...);
    }
};

template <class Signature, class Token, class... Args> auto complete_now(Token&& token, Args&&... args) {
    return aw::async_initiate<Token, Signature>(initiate_now<Signature>{}, token,
                                                std::forward<Args>(args)...);
}

void callbackToken() {
    aw::manual_executor ctx;
    int got = 0;
    std::error_code err;
    async_add(ctx.get_executor(), 2, 3, false, [&](std::error_code ec, int v) {
        err = ec;
        got = v;
    });
    AW_CHECK(got == 0);
    ctx.run();
    AW_CHECK(got == 5 && !err);
}

void useFutureForms() {
    std::future<void> f0 = complete_now<void()>(aw::use_future);
    f0.get();
    std::future<int> f1 = complete_now<void(int)>(aw::use_future, 7);
    AW_CHECK(f1.get() == 7);
    std::future<std::tuple<int, std::string>> f2 =
        complete_now<void(int, std::string)>(aw::use_future, 1, std::string{"x"});
    AW_CHECK((f2.get() == std::make_tuple(1, std::string{"x"})));
    std::future<int> f3 =
        complete_now<void(std::error_code, int)>(aw::use_future, std::error_code{}, 9);
    AW_CHECK(f3.get() == 9);
    std::future<void> f4 = complete_now<void(std::error_code)>(
        aw::use_future, std::error_code{aw::error::operation_aborted});
    bool aborted = false;
    try {
        f4.get();
    } catch (std::system_error const& e) {
        aborted = e.code() == aw::error::operation_aborted;
    }
    AW_CHECK(aborted);
    std::future<int> f5 = complete_now<void(std::exception_ptr, int)>(
        aw::use_future, std::make_exception_ptr(std::runtime_error{"bad"}), 0);
    bool rethrown = false;
    try {
        static_cast<void>(f5.get());
    } catch (std::runtime_error const& e) {
        rethrown = std::string{e.what()} == "bad";
    }
    AW_CHECK(rethrown);
    std::future<std::tuple<int, int>> f6 = complete_now<void(std::exception_ptr, int, int)>(
        aw::use_future, std::exception_ptr{}, 1, 2);
    AW_CHECK((f6.get() == std::make_tuple(1, 2)));
}

void detachedRethrowsExceptionsOnly() {
    complete_now<void(std::error_code, int)>(aw::detached,
                                             std::error_code{aw::error::operation_aborted}, 1);
    complete_now<void()>(aw::detached);
    bool rethrown = false;
    try {
        complete_now<void(std::exception_ptr)>(aw::detached,
                                               std::make_exception_ptr(std::logic_error{"x"}));
    } catch (std::logic_error const&) {
        rethrown = true;
    }
    AW_CHECK(rethrown);
}

void bindExecutorRoutesTheCompletion() {
    aw::manual_executor io;
    aw::manual_executor other;
    bool on_other = false;
    async_add(io.get_executor(), 1, 1, false,
              aw::bind_executor(other.get_executor(), [&](std::error_code, int) {
                  on_other = other.get_executor().running_in_this_thread();
              }));
    io.run();
    AW_CHECK(!on_other && other.pending() == 1U);
    other.run();
    AW_CHECK(on_other);
}

void bindCancellationSlotIsAssociated() {
    aw::cancellation_signal signal;
    auto bound = aw::bind_cancellation_slot(signal.slot(), [](int) {});
    AW_CHECK(aw::get_associated_cancellation_slot(bound) == signal.slot());
    AW_CHECK(!aw::get_associated_cancellation_slot([](int) {}).is_connected());
    aw::manual_executor ctx;
    auto both = aw::bind_executor(ctx.get_executor(), aw::bind_cancellation_slot(signal.slot(), [](int) {}));
    AW_CHECK(aw::get_associated_cancellation_slot(both) == signal.slot());
    AW_CHECK(aw::get_associated_executor(both, ex_t{}) == ctx.get_executor());
}

void postAndDispatchTokens() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::vector<std::string> trace;
    aw::post(ex, [&] { trace.push_back("posted"); });
    aw::dispatch(ex, [&] { trace.push_back("dispatched-from-outside"); });
    AW_CHECK(trace.empty());
    ex.post([&] {
        aw::dispatch(ex, [&] { trace.push_back("inline"); });
        aw::post(ex, [&] { trace.push_back("later"); });
        trace.push_back("end-of-task");
    });
    ctx.run();
    AW_CHECK((trace == std::vector<std::string>{"posted", "dispatched-from-outside", "inline",
                                                "end-of-task", "later"}));
    std::future<void> done = aw::post(ex, aw::use_future);
    ctx.run();
    done.get();
}

void strandAsHandlerExecutor() {
    aw::manual_executor ctx;
    auto s = aw::make_strand(ctx.get_executor());
    bool in_strand = false;
    aw::post(ctx.get_executor(), aw::bind_executor(s, [&] { in_strand = s.running_in_this_thread(); }));
    ctx.run();
    AW_CHECK(in_strand);
}

} // namespace

int main() {
    callbackToken();
    useFutureForms();
    detachedRethrowsExceptionsOnly();
    bindExecutorRoutesTheCompletion();
    bindCancellationSlotIsAssociated();
    postAndDispatchTokens();
    strandAsHandlerExecutor();
    return 0;
}
