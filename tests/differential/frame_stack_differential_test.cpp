// §14.2：帧栈原语的差分测试（C++20）。一个只依赖协程协议的最小执行线程（push / pop、
// await_suspend 返回 void、after_suspend、final 时 pop、pump），promise 与 awaiter 代码以 Lib
// 参数化，分别由 co2 宏协程和原生 co_await 协程驱动，逐事件比对追踪序列。

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"

namespace {

struct log_t {
    std::vector<std::string> events;
    void add(std::string e) { events.push_back(std::move(e)); }
};

struct Co2Lib {
    template <class P = void> using Handle = co2::coroutine_handle<P>;
    using SuspendAlways = co2::suspend_always;
};
struct StdLib {
    template <class P = void> using Handle = std::coroutine_handle<P>;
    using SuspendAlways = std::suspend_always;
};

// 局部对象：析构时打点，用来比对销毁顺序。
struct noisy {
    log_t* log{nullptr};
    std::string name;
    void bind(log_t& l, std::string n) {
        log = &l;
        name = std::move(n);
    }
    ~noisy() {
        if (log != nullptr) log->add(name + " destroyed");
    }
};

template <class Lib> struct frame_base;

template <class Lib> struct mini_thread {
    frame_base<Lib>* top{nullptr};
    bool detached{false};
    log_t* log{nullptr};
    // asio pump：反复恢复栈顶，直到栈空或执行线程被移走。
    void pump() {
        detached = false;
        while (top != nullptr && !detached) top->resume();
        log->add(top == nullptr ? "pump: stack empty" : "pump: detached");
    }
};

struct resume_context {
    void (*fn)(void*){nullptr};
    void* arg{nullptr};
};

template <class Lib> struct frame_base {
    typename Lib::template Handle<> coro{};
    frame_base* caller{nullptr};
    mini_thread<Lib>* thread{nullptr};
    resume_context* rc{nullptr};
    std::exception_ptr pending{};
    std::string name;

    void resume() {
        resume_context context;
        rc = &context;
        thread->log->add("resume " + name);
        coro.resume();
        if (context.fn != nullptr) context.fn(context.arg);
    }
    void push_frame(frame_base* parent) {
        caller = parent;
        thread = parent->thread;
        thread->top = this;
        thread->log->add("push " + name + " over " + parent->name);
    }
    void pop_frame() {
        thread->log->add("pop " + name);
        thread->top = caller;
    }
    void after_suspend(void (*fn)(void*), void* arg) {
        rc->fn = fn;
        rc->arg = arg;
    }
};

template <class Lib> struct task {
    struct promise_type : frame_base<Lib> {
        int value{0};
        explicit promise_type() = default;
        template <class... Args> explicit promise_type(log_t&, std::string n, Args&&...) { this->name = std::move(n); }
        task get_return_object() {
            this->coro = Lib::template Handle<promise_type>::from_promise(*this);
            return task{this};
        }
        typename Lib::SuspendAlways initial_suspend() const noexcept { return {}; }
        struct final_awaiter {
            promise_type* self;
            bool await_ready() const noexcept { return false; }
            void await_suspend(typename Lib::template Handle<>) noexcept { self->pop_frame(); }
            void await_resume() const noexcept {}
        };
        final_awaiter final_suspend() noexcept { return final_awaiter{this}; }
        void return_value(int v) { value = v; }
        void unhandled_exception() { this->pending = std::current_exception(); }
    };

    explicit task(promise_type* p) : frame{p} {}
    task(task&& other) noexcept : frame{other.frame} { other.frame = nullptr; }
    task(task const&) = delete;
    ~task() {
        if (frame != nullptr) frame->coro.destroy();
    }

    // 等待子 task：压栈，返回 void 交回 pump。
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(typename Lib::template Handle<P> h) { frame->push_frame(&h.promise()); }
    int await_resume() {
        if (frame->pending) std::rethrow_exception(frame->pending);
        return frame->value;
    }

    promise_type* frame;
};

// 异步操作：await_suspend 只登记 after_suspend；挂起完成后执行线程被“移走”，由驱动方完成。
template <class Lib> struct pending_ops {
    std::vector<frame_base<Lib>*> waiting;
    int next_value{100};
};

template <class Lib> struct async_op {
    pending_ops<Lib>* ops;
    frame_base<Lib>* frame{nullptr};
    int result{0};
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(typename Lib::template Handle<P> h) {
        frame = &h.promise();
        frame->thread->log->add("op await_suspend in " + frame->name);
        frame->after_suspend(&on_suspend, this);
    }
    static void on_suspend(void* arg) {
        auto* self = static_cast<async_op*>(arg);
        self->frame->thread->log->add("after_suspend: initiate");
        self->frame->thread->detached = true;
        self->ops->waiting.push_back(self->frame);
        self->result = self->ops->next_value++;
    }
    int await_resume() { return result; }
};

template <class Lib> void complete_all(mini_thread<Lib>& thread, pending_ops<Lib>& ops) {
    while (!ops.waiting.empty()) {
        ops.waiting.erase(ops.waiting.begin());
        thread.log->add("complete");
        thread.pump();
    }
}

using C = task<Co2Lib>;
using S = task<StdLib>;

// ---- 场景 1：三层嵌套，压栈出栈顺序 ----
auto co2_leaf(log_t& log, std::string n, int v) CO2_BEG(C, (log, n, v)) {
    log.add(n + " body");
    CO2_RETURN(v);
}
CO2_END
auto co2_mid(log_t& log, std::string n, int v) CO2_BEG(C, (log, n, v), int r{};) {
    log.add(n + " before");
    CO2_AWAIT_SET(r, co2_leaf(log, n + ".leaf", v));
    log.add(n + " after " + std::to_string(r));
    CO2_RETURN(r + 1);
}
CO2_END
auto co2_root(log_t& log, std::string n, int v) CO2_BEG(C, (log, n, v), int r{};) {
    log.add(n + " before");
    CO2_AWAIT_SET(r, co2_mid(log, n + ".mid", v));
    log.add(n + " after " + std::to_string(r));
    CO2_RETURN(r + 1);
}
CO2_END

S std_leaf(log_t& log, std::string n, int v) {
    log.add(n + " body");
    co_return v;
}
S std_mid(log_t& log, std::string n, int v) {
    log.add(n + " before");
    int r = co_await std_leaf(log, n + ".leaf", v);
    log.add(n + " after " + std::to_string(r));
    co_return r + 1;
}
S std_root(log_t& log, std::string n, int v) {
    log.add(n + " before");
    int r = co_await std_mid(log, n + ".mid", v);
    log.add(n + " after " + std::to_string(r));
    co_return r + 1;
}

// ---- 场景 2：叶子抛出，逐层经 await_resume 重抛到根 ----
auto co2_thrower(log_t& log, std::string n, int) CO2_BEG(C, (log, n)) {
    log.add(n + " throws");
    throw std::runtime_error{"leaf"};
}
CO2_END
auto co2_propagate(log_t& log, std::string n, int) CO2_BEG(C, (log, n), int r{};) {
    CO2_AWAIT_SET(r, co2_thrower(log, n + ".leaf", 0));
    log.add("unreachable");
    CO2_RETURN(r);
}
CO2_END
S std_thrower(log_t& log, std::string n, int) {
    log.add(n + " throws");
    throw std::runtime_error{"leaf"};
    co_return 0;
}
S std_propagate(log_t& log, std::string n, int) {
    int r = co_await std_thrower(log, n + ".leaf", 0);
    log.add("unreachable");
    co_return r;
}

// ---- 场景 3：中间层等待异步操作：after_suspend、pump 退出、完成后重新 pump ----
template <class Lib> pending_ops<Lib>* g_ops = nullptr;

auto co2_waits(log_t& log, std::string n, int) CO2_BEG(C, (log, n), int a{}; int b{};) {
    CO2_AWAIT_SET(a, (async_op<Co2Lib>{g_ops<Co2Lib>}));
    log.add(n + " got " + std::to_string(a));
    CO2_AWAIT_SET(b, (async_op<Co2Lib>{g_ops<Co2Lib>}));
    log.add(n + " got " + std::to_string(b));
    CO2_RETURN(a + b);
}
CO2_END
auto co2_outer(log_t& log, std::string n, int) CO2_BEG(C, (log, n), int r{};) {
    CO2_AWAIT_SET(r, co2_waits(log, n + ".waits", 0));
    log.add(n + " sum " + std::to_string(r));
    CO2_RETURN(r);
}
CO2_END
S std_waits(log_t& log, std::string n, int) {
    int a = co_await async_op<StdLib>{g_ops<StdLib>};
    log.add(n + " got " + std::to_string(a));
    int b = co_await async_op<StdLib>{g_ops<StdLib>};
    log.add(n + " got " + std::to_string(b));
    co_return a + b;
}
S std_outer(log_t& log, std::string n, int) {
    int r = co_await std_waits(log, n + ".waits", 0);
    log.add(n + " sum " + std::to_string(r));
    co_return r;
}

// ---- 场景 4：挂起在异步操作上时销毁根帧：子帧随父帧的 awaiter 一起销毁 ----
auto co2_local_leaf(log_t& log, std::string n, int) CO2_BEG(C, (log, n), noisy guard{}; int a{};) {
    guard.bind(log, n + ".guard");
    CO2_AWAIT_SET(a, (async_op<Co2Lib>{g_ops<Co2Lib>}));
    CO2_RETURN(a);
}
CO2_END
auto co2_local_root(log_t& log, std::string n, int) CO2_BEG(C, (log, n), noisy guard{}; int r{};) {
    guard.bind(log, n + ".guard");
    CO2_AWAIT_SET(r, co2_local_leaf(log, n + ".leaf", 0));
    CO2_RETURN(r);
}
CO2_END
S std_local_leaf(log_t& log, std::string n, int) {
    noisy guard;
    guard.bind(log, n + ".guard");
    int a = co_await async_op<StdLib>{g_ops<StdLib>};
    co_return a;
}
S std_local_root(log_t& log, std::string n, int) {
    noisy guard;
    guard.bind(log, n + ".guard");
    int r = co_await std_local_leaf(log, n + ".leaf", 0);
    co_return r;
}

// 驱动：根帧放到栈底，pump；有挂起的操作就逐个完成。
template <class Lib, class Make> log_t drive(Make make, bool destroy_while_suspended) {
    log_t log;
    pending_ops<Lib> ops;
    g_ops<Lib> = &ops;
    mini_thread<Lib> thread;
    thread.log = &log;
    {
        task<Lib> root = make(log, std::string{"root"}, 3);
        root.frame->thread = &thread;
        thread.top = root.frame;
        thread.pump();
        if (destroy_while_suspended) {
            log.add("-- destroy root while suspended");
        } else {
            complete_all(thread, ops);
            if (root.frame->pending) {
                try {
                    std::rethrow_exception(root.frame->pending);
                } catch (std::exception const& e) {
                    log.add(std::string{"root exception: "} + e.what());
                }
            } else {
                log.add("root value " + std::to_string(root.frame->value));
            }
        }
    }
    log.add("-- root destroyed");
    return log;
}

void compare(char const* name, log_t const& a, log_t const& b) {
    if (std::getenv("AW_DIFF_VERBOSE") != nullptr) {
        std::cout << "== " << name << '\n';
        for (auto const& e : a.events) std::cout << "    " << e << '\n';
    }
    if (a.events == b.events) return;
    std::cerr << "differential mismatch in scenario " << name << "\n  co2:\n";
    for (auto const& e : a.events) std::cerr << "    " << e << '\n';
    std::cerr << "  std:\n";
    for (auto const& e : b.events) std::cerr << "    " << e << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main() {
    compare("nesting", drive<Co2Lib>(co2_root, false), drive<StdLib>(std_root, false));
    compare("exception", drive<Co2Lib>(co2_propagate, false), drive<StdLib>(std_propagate, false));
    compare("after_suspend", drive<Co2Lib>(co2_outer, false), drive<StdLib>(std_outer, false));
    compare("destroy_suspended", drive<Co2Lib>(co2_local_root, true), drive<StdLib>(std_local_root, true));
    // 防止“两边都什么也没记”的空比对。
    if (drive<Co2Lib>(co2_root, false).events.size() < 10U) return EXIT_FAILURE;
    return 0;
}
