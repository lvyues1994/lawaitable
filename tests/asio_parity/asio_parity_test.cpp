// §14.3：同一场景写两份——asio 原生协程（C++20）与本库的宏协程——各自记录事件，断言相同。

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <awaitable.hpp>

#include "test_cancellable_ops.hpp"
#include "test_ops.hpp"

namespace {

struct trace_t {
    std::vector<std::string> events;
    void add(std::string e) { events.push_back(std::move(e)); }
};

void compare(std::string const& name, trace_t const& ours, trace_t const& theirs) {
    if (ours.events == theirs.events && !ours.events.empty()) return;
    std::cerr << "asio parity mismatch in scenario " << name << "\n  aw:\n";
    for (auto const& e : ours.events) std::cerr << "    " << e << '\n';
    std::cerr << "  asio:\n";
    for (auto const& e : theirs.events) std::cerr << "    " << e << '\n';
    std::exit(EXIT_FAILURE);
}

std::string describe(std::exception_ptr e) {
    if (!e) return "none";
    try {
        std::rethrow_exception(e);
    } catch (asio::multiple_exceptions const& m) {
        return "multiple_exceptions(" + describe(m.first_exception()) + ")";
    } catch (aw::multiple_exceptions const& m) {
        return "multiple_exceptions(" + describe(m.first_exception()) + ")";
    } catch (std::system_error const& s) {
        return s.code() == std::error_code{asio::error::operation_aborted} ||
                       s.code() == std::error_code{aw::error::operation_aborted}
                   ? std::string{"operation_aborted"}
                   : std::string{"system_error"};
    } catch (std::runtime_error const& r) {
        return std::string{"runtime_error "} + r.what();
    } catch (...) {
        return "unknown";
    }
}

// io_context::poll 不一定执行它自己运行期间新投递的项：反复 poll 直到没有就绪项。
void drain(asio::io_context& io) {
    for (;;) {
        io.restart();
        if (io.poll() == 0U) break;
    }
}

using aw_ex = aw::manual_executor::executor_type;
template <class T> using aw_task = aw::awaitable<T, aw_ex>;

// ============================================================================
// 场景 1：三层嵌套，每层前后打点
// ============================================================================
asio::awaitable<int> asio_level(trace_t& t, int depth) {
    t.add("enter " + std::to_string(depth));
    int r = depth;
    if (depth < 3) r += co_await asio_level(t, depth + 1);
    t.add("leave " + std::to_string(depth));
    co_return r;
}
auto aw_level(trace_t& t, int depth) CO2_BEG(aw_task<int>, (t, depth), int r{};) {
    t.add("enter " + std::to_string(depth));
    r = depth;
    if (depth < 3) {
        CO2_AWAIT_SET(r, aw_level(t, depth + 1));
        r += depth;
    }
    t.add("leave " + std::to_string(depth));
    CO2_RETURN(r);
}
CO2_END

void scenario_nesting() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        aw::co_spawn(ctx, aw_level(a, 1), [&](std::exception_ptr e, int v) {
            a.add("cb " + describe(e) + " " + std::to_string(v));
        });
        ctx.run();
    }
    {
        asio::io_context io;
        asio::co_spawn(io, asio_level(b, 1), [&](std::exception_ptr e, int v) {
            b.add("cb " + describe(e) + " " + std::to_string(v));
        });
        io.run();
    }
    compare("nesting", a, b);
}

// ============================================================================
// 场景 2：子帧抛出，父帧不处理，一路传到 co_spawn 回调
// ============================================================================
asio::awaitable<int> asio_thrower(trace_t& t) {
    t.add("throwing");
    throw std::runtime_error{"leaf"};
    co_return 0;
}
asio::awaitable<int> asio_middle(trace_t& t) {
    int v = co_await asio_thrower(t);
    t.add("unreachable");
    co_return v;
}
auto aw_thrower(trace_t& t) CO2_BEG(aw_task<int>, (t)) {
    t.add("throwing");
    throw std::runtime_error{"leaf"};
}
CO2_END
auto aw_middle(trace_t& t) CO2_BEG(aw_task<int>, (t), int v{};) {
    CO2_AWAIT_SET(v, aw_thrower(t));
    t.add("unreachable");
    CO2_RETURN(v);
}
CO2_END

void scenario_exception() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        aw::co_spawn(ctx, aw_middle(a), [&](std::exception_ptr e, int) { a.add("cb " + describe(e)); });
        ctx.run();
    }
    {
        asio::io_context io;
        asio::co_spawn(io, asio_middle(b), [&](std::exception_ptr e, int) { b.add("cb " + describe(e)); });
        io.run();
    }
    compare("exception", a, b);
}

// ============================================================================
// 场景 3：三级取消 × 三种过滤器 × throw_if_cancelled 开 / 关，以及 reset 之后
// ============================================================================
enum class filter_kind { keep_default, partial, total };

asio::awaitable<void> asio_cancel_body(trace_t& t, filter_kind f, bool tic, asio::steady_timer& timer) {
    if (f == filter_kind::partial)
        co_await asio::this_coro::reset_cancellation_state(asio::enable_partial_cancellation());
    if (f == filter_kind::total)
        co_await asio::this_coro::reset_cancellation_state(asio::enable_total_cancellation());
    if (!tic) co_await asio::this_coro::throw_if_cancelled(false);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    t.add(ec == asio::error::operation_aborted ? "wait aborted" : "wait ok");
    asio::cancellation_state cs = co_await asio::this_coro::cancellation_state;
    t.add("cancelled " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    t.add("post done");
    co_await asio::this_coro::reset_cancellation_state();
    cs = co_await asio::this_coro::cancellation_state;
    t.add("after reset " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    t.add("post after reset done");
}

using aw_event = aw_test::manual_event<aw_ex>;
auto aw_cancel_body(trace_t& t, filter_kind f, bool tic, aw_event* ev, aw_ex ex)
    CO2_BEG(aw_task<void>, (t, f, tic, ev, ex), std::tuple<std::error_code> r{}; aw::cancellation_state cs{};) {
    if (f == filter_kind::partial)
        CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_partial_cancellation{}));
    if (f == filter_kind::total)
        CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_total_cancellation{}));
    if (!tic) CO2_AWAIT(aw::this_coro::throw_if_cancelled(false));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    t.add(std::get<0>(r) == std::error_code{aw::error::operation_aborted} ? "wait aborted" : "wait ok");
    CO2_AWAIT_SET(cs, aw::this_coro::cancellation_state);
    t.add("cancelled " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    t.add("post done");
    CO2_AWAIT(aw::this_coro::reset_cancellation_state());
    CO2_AWAIT_SET(cs, aw::this_coro::cancellation_state);
    t.add("after reset " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    t.add("post after reset done");
    CO2_RETURN();
}
CO2_END

void scenario_cancellation() {
    aw::cancellation_type const aw_types[] = {aw::cancellation_type::terminal, aw::cancellation_type::partial,
                                              aw::cancellation_type::total};
    asio::cancellation_type const asio_types[] = {asio::cancellation_type::terminal,
                                                  asio::cancellation_type::partial,
                                                  asio::cancellation_type::total};
    filter_kind const filters[] = {filter_kind::keep_default, filter_kind::partial, filter_kind::total};
    for (int ti = 0; ti != 3; ++ti) {
        for (filter_kind f : filters) {
            for (bool tic : {true, false}) {
                std::string const name = "cancel type=" + std::to_string(ti) + " filter=" +
                                         std::to_string(static_cast<int>(f)) + " tic=" + std::to_string(tic);
                trace_t a, b;
                {
                    aw::manual_executor ctx;
                    aw_event ev{ctx.get_executor()};
                    aw::cancellation_signal sig;
                    aw::co_spawn(ctx, aw_cancel_body(a, f, tic, &ev, ctx.get_executor()),
                                 aw::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e) {
                                     a.add("cb " + describe(e));
                                 }));
                    ctx.run();
                    sig.emit(aw_types[ti]);
                    a.add("emitted");
                    ctx.run();
                    if (ev.waiting()) {
                        a.add("still waiting");
                        sig.emit(aw::cancellation_type::terminal);
                        ctx.run();
                    }
                }
                {
                    asio::io_context io;
                    asio::steady_timer timer{io, asio::steady_timer::time_point::max()};
                    asio::cancellation_signal sig;
                    bool waited = false;
                    asio::co_spawn(io, asio_cancel_body(b, f, tic, timer),
                                   asio::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e) {
                                       b.add("cb " + describe(e));
                                   }));
                    drain(io);
                    sig.emit(asio_types[ti]);
                    b.add("emitted");
                    drain(io);
                    waited = std::find(b.events.begin(), b.events.end(), "wait aborted") != b.events.end() ||
                             std::find(b.events.begin(), b.events.end(), "wait ok") != b.events.end();
                    if (!waited) {
                        b.add("still waiting");
                        sig.emit(asio::cancellation_type::terminal);
                        drain(io);
                    }
                }
                compare(name, a, b);
            }
        }
    }
}

// ============================================================================
// 场景 4：N 个执行线程绑定同一个 strand，交替等待异步操作：互不重叠，顺序一致
// ============================================================================
constexpr int strand_rounds = 50;

asio::awaitable<void> asio_strand_worker(int id, asio::strand<asio::thread_pool::executor_type> s,
                                         std::vector<std::string>* order, std::atomic<int>* inside,
                                         std::atomic<bool>* overlap) {
    for (int k = 0; k != strand_rounds; ++k) {
        if (inside->fetch_add(1) != 0) overlap->store(true);
        order->push_back(std::to_string(id) + ":" + std::to_string(k));
        inside->fetch_sub(1);
        co_await asio::post(s, asio::use_awaitable);
    }
}

using pool_strand = aw::strand<aw::thread_pool::executor_type>;
auto aw_strand_worker(int id, pool_strand s, std::vector<std::string>* order, std::atomic<int>* inside,
                      std::atomic<bool>* overlap)
    CO2_BEG((aw::awaitable<void, pool_strand>), (id, s, order, inside, overlap), int k{};) {
    for (k = 0; k != strand_rounds; ++k) {
        if (inside->fetch_add(1) != 0) overlap->store(true);
        order->push_back(std::to_string(id) + ":" + std::to_string(k));
        inside->fetch_sub(1);
        CO2_AWAIT(aw::post(s, aw::use_awaitable));
    }
    CO2_RETURN();
}
CO2_END

void scenario_strand() {
    trace_t a, b;
    {
        aw::thread_pool pool{4U};
        pool_strand s = aw::make_strand(pool.get_executor());
        std::vector<std::string> order;
        std::atomic<int> inside{0};
        std::atomic<bool> overlap{false};
        std::promise<void> all_done;
        std::atomic<int> remaining{3};
        // 在 strand 内发起三个执行线程：co_spawn 先 dispatch，所以启动在 strand 内内联进行。
        s.post([&] {
            for (int id = 0; id != 3; ++id)
                aw::co_spawn(s, aw_strand_worker(id, s, &order, &inside, &overlap), [&](std::exception_ptr) {
                    if (remaining.fetch_sub(1) == 1) all_done.set_value();
                });
        });
        all_done.get_future().wait();
        for (auto const& e : order) a.add(e);
        a.add(overlap.load() ? "overlap" : "no overlap");
    }
    {
        asio::thread_pool pool{4};
        auto s = asio::make_strand(pool.get_executor());
        std::vector<std::string> order;
        std::atomic<int> inside{0};
        std::atomic<bool> overlap{false};
        asio::post(s, [&] {
            for (int id = 0; id != 3; ++id)
                asio::co_spawn(s, asio_strand_worker(id, s, &order, &inside, &overlap), asio::detached);
        });
        pool.join();
        for (auto const& e : order) b.add(e);
        b.add(overlap.load() ? "overlap" : "no overlap");
    }
    compare("strand", a, b);
}

// ============================================================================
// 场景 5：|| / &&：先完成者胜出、败者被取消；单边与双边失败；void 与非 void；三元链式
// ============================================================================
asio::awaitable<int> asio_delay(int n) {
    for (int i = 0; i != n; ++i) co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    co_return n;
}
asio::awaitable<void> asio_void_delay(int n) {
    for (int i = 0; i != n; ++i) co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
}
asio::awaitable<int> asio_fail(int n) {
    for (int i = 0; i != n; ++i) co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    throw std::runtime_error{"fail " + std::to_string(n)};
    co_return 0;
}

asio::awaitable<void> asio_operators(trace_t& t) {
    using namespace asio::experimental::awaitable_operators;
    auto r1 = co_await (asio_delay(1) || asio_delay(3));
    t.add("or index " + std::to_string(r1.index()) + " value " + std::to_string(std::get<0>(r1)));
    auto r2 = co_await (asio_delay(1) && asio_delay(3));
    t.add("and " + std::to_string(std::get<0>(r2)) + "," + std::to_string(std::get<1>(r2)));
    int r3 = co_await (asio_void_delay(1) && asio_delay(2));
    t.add("void and int " + std::to_string(r3));
    auto r4 = co_await (asio_delay(1) && asio_delay(2) && asio_delay(3));
    t.add("and3 " + std::to_string(std::get<0>(r4) + std::get<1>(r4) + std::get<2>(r4)));
    auto r5 = co_await (asio_fail(1) || asio_delay(2) || asio_delay(3));
    t.add("or3 index " + std::to_string(r5.index()) + " value " + std::to_string(std::get<1>(r5)));
    auto r6 = co_await (asio_void_delay(2) || asio_delay(1));
    t.add("void or int index " + std::to_string(r6.index()));
}
asio::awaitable<void> asio_and_one_fails() {
    using namespace asio::experimental::awaitable_operators;
    co_await (asio_fail(1) && asio_delay(3));
}
asio::awaitable<void> asio_and_both_fail() {
    using namespace asio::experimental::awaitable_operators;
    co_await (asio_fail(1) && asio_fail(2));
}
asio::awaitable<void> asio_or_both_fail() {
    using namespace asio::experimental::awaitable_operators;
    co_await (asio_fail(2) || asio_fail(1));
}

auto aw_delay(int n, aw_ex ex) CO2_BEG(aw_task<int>, (n, ex), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    CO2_RETURN(n);
}
CO2_END
auto aw_void_delay(int n, aw_ex ex) CO2_BEG(aw_task<void>, (n, ex), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    CO2_RETURN();
}
CO2_END
auto aw_fail(int n, aw_ex ex) CO2_BEG(aw_task<int>, (n, ex), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    throw std::runtime_error{"fail " + std::to_string(n)};
}
CO2_END

using namespace aw::awaitable_operators;

auto aw_operators(trace_t& t, aw_ex ex)
    CO2_BEG(aw_task<void>, (t, ex), aw::variant<int, int> r1{}; std::tuple<int, int> r2{}; int r3{};
            std::tuple<int, int, int> r4{}; aw::variant<int, int, int> r5{}; aw::variant<aw::monostate, int> r6{};) {
    CO2_AWAIT_SET(r1, aw_delay(1, ex) || aw_delay(3, ex));
    t.add("or index " + std::to_string(r1.index()) + " value " + std::to_string(aw::get<0>(r1)));
    CO2_AWAIT_SET(r2, aw_delay(1, ex) && aw_delay(3, ex));
    t.add("and " + std::to_string(std::get<0>(r2)) + "," + std::to_string(std::get<1>(r2)));
    CO2_AWAIT_SET(r3, aw_void_delay(1, ex) && aw_delay(2, ex));
    t.add("void and int " + std::to_string(r3));
    CO2_AWAIT_SET(r4, aw_delay(1, ex) && aw_delay(2, ex) && aw_delay(3, ex));
    t.add("and3 " + std::to_string(std::get<0>(r4) + std::get<1>(r4) + std::get<2>(r4)));
    CO2_AWAIT_SET(r5, aw_fail(1, ex) || aw_delay(2, ex) || aw_delay(3, ex));
    t.add("or3 index " + std::to_string(r5.index()) + " value " + std::to_string(aw::get<1>(r5)));
    CO2_AWAIT_SET(r6, aw_void_delay(2, ex) || aw_delay(1, ex));
    t.add("void or int index " + std::to_string(r6.index()));
    CO2_RETURN();
}
CO2_END
auto aw_and_one_fails(aw_ex ex) CO2_BEG(aw_task<void>, (ex)) {
    CO2_AWAIT(aw_fail(1, ex) && aw_delay(3, ex));
    CO2_RETURN();
}
CO2_END
auto aw_and_both_fail(aw_ex ex) CO2_BEG(aw_task<void>, (ex)) {
    CO2_AWAIT(aw_fail(1, ex) && aw_fail(2, ex));
    CO2_RETURN();
}
CO2_END
auto aw_or_both_fail(aw_ex ex) CO2_BEG(aw_task<void>, (ex)) {
    CO2_AWAIT(aw_fail(2, ex) || aw_fail(1, ex));
    CO2_RETURN();
}
CO2_END

void scenario_operators() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        aw_ex ex = ctx.get_executor();
        auto cb = [&](std::exception_ptr e) { a.add("cb " + describe(e)); };
        aw::co_spawn(ctx, aw_operators(a, ex), cb);
        ctx.run();
        aw::co_spawn(ctx, aw_and_one_fails(ex), cb);
        ctx.run();
        aw::co_spawn(ctx, aw_and_both_fail(ex), cb);
        ctx.run();
        aw::co_spawn(ctx, aw_or_both_fail(ex), cb);
        ctx.run();
    }
    {
        asio::io_context io;
        auto cb = [&](std::exception_ptr e) { b.add("cb " + describe(e)); };
        asio::co_spawn(io, asio_operators(b), cb);
        io.run();
        io.restart();
        asio::co_spawn(io, asio_and_one_fails(), cb);
        io.run();
        io.restart();
        asio::co_spawn(io, asio_and_both_fail(), cb);
        io.run();
        io.restart();
        asio::co_spawn(io, asio_or_both_fail(), cb);
        io.run();
    }
    compare("operators", a, b);
}

// ============================================================================
// 场景 6：异步操作在别的线程完成，恢复时 running_in_this_thread() 为真
// ============================================================================
template <class Token> auto asio_from_foreign_thread(aw_test::foreign_threads& threads, int v, Token&& token) {
    return asio::async_initiate<Token, void(std::error_code, int)>(
        [&threads, v](auto handler) {
            auto ex = asio::get_associated_executor(handler);
            threads.start([ex, v, h = std::move(handler)]() mutable {
                asio::dispatch(ex, [h = std::move(h), v]() mutable { std::move(h)(std::error_code{}, v); });
            });
        },
        token);
}

asio::awaitable<void> asio_affinity(trace_t& t, asio::thread_pool::executor_type ex,
                                    aw_test::foreign_threads& threads) {
    int v = co_await asio_from_foreign_thread(threads, 5, asio::use_awaitable);
    t.add("value " + std::to_string(v) + " on own executor " + std::to_string(ex.running_in_this_thread()));
}

using pool_ex = aw::thread_pool::executor_type;
auto aw_affinity(trace_t& t, pool_ex ex, aw_test::foreign_threads* threads)
    CO2_BEG((aw::awaitable<void, pool_ex>), (t, ex, threads), int v{};) {
    CO2_AWAIT_SET(v, aw_test::async_from_foreign_thread(*threads, 5, aw::use_awaitable));
    t.add("value " + std::to_string(v) + " on own executor " + std::to_string(ex.running_in_this_thread()));
    CO2_RETURN();
}
CO2_END

void scenario_affinity() {
    trace_t a, b;
    {
        aw_test::foreign_threads threads;
        aw::thread_pool pool{2U};
        aw::co_spawn(pool, aw_affinity(a, pool.get_executor(), &threads), aw::use_future).get();
    }
    {
        asio::thread_pool pool{2};
        aw_test::foreign_threads threads; // 在 pool 之前析构：先 join 外部线程，再销毁 pool
        asio::co_spawn(pool, asio_affinity(b, pool.get_executor(), threads), asio::use_future).get();
        pool.join();
    }
    compare("affinity", a, b);
}

// ============================================================================
// 场景 7：co_spawn 启动期同步完成时，完成回调不在 co_spawn 的调用栈内执行
// ============================================================================
asio::awaitable<int> asio_ready() { co_return 1; }
auto aw_ready() CO2_BEG(aw_task<int>, ()) { CO2_RETURN(1); }
CO2_END

void scenario_launch_completion() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        ctx.get_executor().post([&] {
            aw::co_spawn(ctx, aw_ready(), [&](std::exception_ptr, int v) { a.add("cb " + std::to_string(v)); });
            a.add("after co_spawn");
        });
        ctx.run();
    }
    {
        asio::io_context io;
        asio::post(io, [&] {
            asio::co_spawn(io, asio_ready(), [&](std::exception_ptr, int v) { b.add("cb " + std::to_string(v)); });
            b.add("after co_spawn");
        });
        io.run();
    }
    compare("launch completion", a, b);
}

} // namespace

int main() {
    scenario_nesting();
    scenario_exception();
    scenario_cancellation();
    scenario_strand();
    scenario_operators();
    scenario_affinity();
    scenario_launch_completion();
    std::cout << "asio parity: all scenarios match\n";
    return 0;
}
