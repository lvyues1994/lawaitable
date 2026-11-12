#pragma once

// 测试用异步操作。全部遵守 §8.1 的操作作者契约：完成时经回调的关联 executor 调用回调。

#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw_test {

// 把回调和参数绑成一个无参可调用对象。
template <class Handler, class... Vs> struct bound_completion {
    Handler handler;
    std::tuple<Vs...> values;
    void operator()() { call(std::index_sequence_for<Vs...>{}); }
    template <std::size_t... I> void call(std::index_sequence<I...>) {
        handler(std::move(std::get<I>(values))...);
    }
};

template <class Handler, class... Vs>
bound_completion<typename std::decay<Handler>::type, typename std::decay<Vs>::type...>
bind_completion(Handler&& handler, Vs&&... values) {
    return {std::forward<Handler>(handler),
            std::tuple<typename std::decay<Vs>::type...>{std::forward<Vs>(values)...}};
}

// 在 io_ex 上执行，然后 dispatch 到回调的关联 executor（asio 的 post + work_dispatcher）。
template <class Target, class Completion> struct dispatch_to {
    Target target;
    Completion completion;
    void operator()() { target.dispatch(std::move(completion)); }
};

template <class Executor> struct initiate_complete_later {
    Executor io_ex;
    template <class Handler, class... Vs> void operator()(Handler&& handler, Vs&&... values) const {
        auto target = aw::get_associated_executor(handler, io_ex);
        auto completion = bind_completion(std::forward<Handler>(handler), std::forward<Vs>(values)...);
        io_ex.post(dispatch_to<decltype(target), decltype(completion)>{target, std::move(completion)});
    }
};

// void(std::error_code, int)：先 post 到 io_ex，再在回调的 executor 上完成。
template <class Executor, class Token> auto async_value(Executor const& io_ex, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(
        initiate_complete_later<Executor>{io_ex}, token, std::error_code{}, v);
}

// void(std::error_code, int)：以错误完成。
template <class Executor, class Token>
auto async_error(Executor const& io_ex, std::error_code ec, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(
        initiate_complete_later<Executor>{io_ex}, token, ec, 0);
}

// void(std::exception_ptr)。
template <class Executor, class Token>
auto async_exception(Executor const& io_ex, std::exception_ptr e, Token&& token) {
    return aw::async_initiate<Token, void(std::exception_ptr)>(initiate_complete_later<Executor>{io_ex},
                                                               token, e);
}

// void()。
template <class Executor, class Token> auto async_nothing(Executor const& io_ex, Token&& token) {
    return aw::async_initiate<Token, void()>(initiate_complete_later<Executor>{io_ex}, token);
}

// void(int, std::string)：没有错误参数，多个值。
template <class Executor, class Token>
auto async_pair(Executor const& io_ex, int a, std::string b, Token&& token) {
    return aw::async_initiate<Token, void(int, std::string)>(initiate_complete_later<Executor>{io_ex},
                                                             token, a, std::move(b));
}

// void(std::error_code, int)：initiation 在自己的栈里同步调用回调，前后打点。
struct initiate_immediately {
    std::vector<std::string>* trace;
    template <class Handler> void operator()(Handler&& handler, int v) const {
        trace->push_back("init-begin");
        handler(std::error_code{}, v);
        trace->push_back("init-end");
    }
};
template <class Token> auto async_immediate(std::vector<std::string>& trace, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_immediately{&trace}, token, v);
}

// void(std::error_code, int)：在一个外部线程上完成（经关联 executor 回到协程的 executor）。
class foreign_threads {
  public:
    foreign_threads() = default;
    foreign_threads(foreign_threads const&) = delete;
    foreign_threads& operator=(foreign_threads const&) = delete;
    ~foreign_threads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            threads.swap(threads_);
        }
        for (auto& t : threads) t.join();
    }
    template <class F> void start(F&& f) {
        std::lock_guard<std::mutex> lock{mutex_};
        threads_.emplace_back(std::forward<F>(f));
    }

  private:
    std::mutex mutex_;
    std::vector<std::thread> threads_;
};

struct initiate_on_foreign_thread {
    foreign_threads* threads;
    template <class Handler> void operator()(Handler&& handler, int v) const {
        using handler_type = typename std::decay<Handler>::type;
        auto target = aw::get_associated_executor(handler, 0);
        threads->start([target, v, h = handler_type(std::forward<Handler>(handler))]() mutable {
            target.dispatch(bind_completion(std::move(h), std::error_code{}, v));
        });
    }
};
template <class Token> auto async_from_foreign_thread(foreign_threads& threads, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_on_foreign_thread{&threads},
                                                                 token, v);
}

} // namespace aw_test
