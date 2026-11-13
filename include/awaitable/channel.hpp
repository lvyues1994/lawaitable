#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/channel_error.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/core/detail/move_only_function.hpp"
#include "awaitable/error.hpp"
#include "awaitable/impl/channel_op.hpp"
// error_code 一律经 make_error_code 构造：GCC 8 + libc++（QNX 7.1）下，从错误码枚举隐式构造会在
// libc++ 的默认实参处触发 -Wzero-as-null-pointer-constant。
#include "awaitable/optional.hpp"

namespace aw {

// channel 规格增补 C3：满队列时的处理。
enum class channel_policy { block, drop_oldest, drop_newest };

// 有界多生产者多消费者 channel（规格增补 C2–C7）。成员函数线程安全（析构与 set_drop_handler 除外）。
// 所有完成都 post 到回调的关联 executor（缺省为 channel 的 executor）。
template <class T, class Executor> class channel {
    static_assert(std::is_object<T>::value && !std::is_const<T>::value, "channel<T>: T must be a non-const object type");
    static_assert(std::is_move_constructible<T>::value, "channel<T>: T must be move constructible");

    using node = detail::channel_node<T>;
    using lock_type = std::unique_lock<std::mutex>;

  public:
    using value_type = T;
    using executor_type = Executor;
    using receive_type = detail::channel_receive_type<T>;

    channel(Executor const& ex, std::size_t capacity, channel_policy policy = channel_policy::block)
        : ex_{ex}, capacity_{capacity}, policy_{policy} {
        // C3 第 1 条：没有缓冲区时“满了就丢”没有意义。
        CO2_CONTRACT_CHECK(capacity != 0U || policy == channel_policy::block);
        ring_.resize(capacity);
    }

    template <class Context, class = decltype(std::declval<Context&>().get_executor())>
    channel(Context& ctx, std::size_t capacity, channel_policy policy = channel_policy::block)
        : channel(ctx.get_executor(), capacity, policy) {}

    channel(channel const&) = delete;
    channel& operator=(channel const&) = delete;

    // C4 第 4 条：挂起的回调被销毁而不调用；不能与其他调用并发。
    ~channel() {
        discard_all(receivers_);
        discard_all(senders_);
    }

    executor_type get_executor() const noexcept { return ex_; }
    std::size_t capacity() const noexcept { return capacity_; }
    channel_policy policy() const noexcept { return policy_; }

    bool is_open() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return open_;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return count_;
    }
    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return dropped_;
    }

    // C3 第 5 条：f(T&&) 在发起发送的线程上、锁外调用；须在并发使用之前设置；抛出即 terminate。
    template <class F> void set_drop_handler(F&& f) { on_drop_ = co2::detail::MoveOnlyFunction<void(T&&)>(std::forward<F>(f)); }

    // 完成签名 void(std::error_code)。
    template <class Token> auto async_send(T value, Token&& token) {
        return async_initiate<Token, void(std::error_code)>(initiate_send{this}, token, std::move(value));
    }

    // 完成签名 void(std::error_code, receive_type)。
    template <class Token> auto async_receive(Token&& token) {
        return async_initiate<Token, void(std::error_code, receive_type)>(initiate_receive{this}, token);
    }

    // 值被 channel 接收（入队、交给接收方或按策略丢弃）返回 true；否则不移走 value。
    bool try_send(T&& value) { return try_send_impl(value); }
    bool try_send(T const& value) {
        T copy(value);
        return try_send_impl(copy);
    }

    aw::optional<T> try_receive() {
        lock_type lock{mutex_};
        aw::optional<T> out;
        node* woken = nullptr;
        if (!take_locked(out, woken)) return out;
        lock.unlock();
        finish(woken);
        return out;
    }

    // C4 第 1 条。幂等。
    void close() {
        lock_type lock{mutex_};
        if (!open_) return;
        open_ = false;
        detail::channel_wait_list<T> done;
        detach_all(receivers_, done, channel_errc::channel_closed);
        detach_all(senders_, done, channel_errc::channel_closed);
        lock.unlock();
        finish_all(done);
    }

    // C4 第 2 条。
    void cancel() {
        lock_type lock{mutex_};
        detail::channel_wait_list<T> done;
        detach_all(receivers_, done, channel_errc::channel_cancelled);
        detach_all(senders_, done, channel_errc::channel_cancelled);
        lock.unlock();
        finish_all(done);
    }

  private:
    // ---- slot 处理器（C4 第 3 条）----
    class cancel_handler {
      public:
        cancel_handler(channel* self, node* n) noexcept : self_{self}, node_{n} {}
        cancel_handler(cancel_handler const&) = delete;
        cancel_handler& operator=(cancel_handler const&) = delete;
        ~cancel_handler() { node_->release(); }

        void operator()(cancellation_type type) {
            if (type == cancellation_type::none) return;
            // 已出队（被完成或被取消）就不再碰 channel：channel 此时可能已被销毁。
            if (!node_->queued.load(std::memory_order_acquire)) return;
            self_->cancel_node(node_);
        }

      private:
        channel* self_;
        node* node_;
    };

    struct initiate_send {
        channel* self;
        template <class Handler> void operator()(Handler&& handler, T value) const {
            self->start_send(std::forward<Handler>(handler), std::move(value));
        }
    };
    struct initiate_receive {
        channel* self;
        template <class Handler> void operator()(Handler&& handler) const {
            self->start_receive(std::forward<Handler>(handler));
        }
    };

    enum class send_status { closed, delivered, buffered, full, dropped };

    // 锁内：尝试放下 value。delivered 时 woken 是拿到值的接收方；dropped 时被丢弃的值移进 discarded。
    send_status send_locked(T& value, node*& woken, aw::optional<T>& discarded) {
        if (!open_) return send_status::closed;
        if (!receivers_.empty()) {
            node* const r = receivers_.pop();
            r->queued.store(false, std::memory_order_release);
            r->value.emplace(std::move(value));
            r->ec = std::error_code{};
            woken = r;
            return send_status::delivered;
        }
        if (count_ < capacity_) {
            push_back(std::move(value));
            return send_status::buffered;
        }
        switch (policy_) {
        case channel_policy::block:
            return send_status::full;
        case channel_policy::drop_oldest:
            discarded.emplace(pop_front());
            push_back(std::move(value));
            break;
        case channel_policy::drop_newest:
            discarded.emplace(std::move(value));
            break;
        }
        ++dropped_;
        return send_status::dropped;
    }

    // 锁内：取一个值。返回 false 表示没有可取的值（空或已关闭且取尽）。
    bool take_locked(aw::optional<T>& out, node*& woken) {
        if (count_ != 0U) {
            out.emplace(pop_front());
            if (!senders_.empty()) { // block 策略：把最早的等待发送方补进队尾
                node* const s = senders_.pop();
                s->queued.store(false, std::memory_order_release);
                push_back(std::move(*s->value));
                s->value.reset();
                s->ec = std::error_code{};
                woken = s;
            }
            return true;
        }
        if (!senders_.empty()) { // 容量 0：直接从发送方取
            node* const s = senders_.pop();
            s->queued.store(false, std::memory_order_release);
            out.emplace(std::move(*s->value));
            s->value.reset();
            s->ec = std::error_code{};
            woken = s;
            return true;
        }
        return false;
    }

    bool try_send_impl(T& value) {
        lock_type lock{mutex_};
        node* woken = nullptr;
        aw::optional<T> discarded;
        send_status const status = send_locked(value, woken, discarded);
        lock.unlock();
        if (status == send_status::closed || status == send_status::full) return false;
        finish(woken);
        handle_dropped(discarded);
        return true;
    }

    template <class Handler> void start_send(Handler&& handler, T value) {
        using handler_type = typename std::decay<Handler>::type;
        lock_type lock{mutex_};
        node* woken = nullptr;
        aw::optional<T> discarded;
        send_status const status = send_locked(value, woken, discarded);
        if (status == send_status::full) {
            node* const n = enqueue<handler_type, true>(std::forward<Handler>(handler), senders_);
            n->value.emplace(std::move(value));
            return;
        }
        lock.unlock();
        finish(woken);
        handle_dropped(discarded);
        std::error_code const ec =
            status == send_status::closed ? make_error_code(channel_errc::channel_closed) : std::error_code{};
        detail::channel_post_send(ex_, std::forward<Handler>(handler), ec);
    }

    template <class Handler> void start_receive(Handler&& handler) {
        using handler_type = typename std::decay<Handler>::type;
        lock_type lock{mutex_};
        aw::optional<T> out;
        node* woken = nullptr;
        if (take_locked(out, woken)) {
            lock.unlock();
            finish(woken);
            detail::channel_post_receive<T>(ex_, std::forward<Handler>(handler), std::error_code{}, std::move(out));
            return;
        }
        if (!open_) {
            lock.unlock();
            detail::channel_post_receive<T>(ex_, std::forward<Handler>(handler),
                                            make_error_code(channel_errc::channel_closed), aw::optional<T>{});
            return;
        }
        enqueue<handler_type, false>(std::forward<Handler>(handler), receivers_);
    }

    // 锁内：建节点、按需在 slot 上装处理器、入队。slot 的使用在操作所在的 executor 上（§6.2 第 7 条）。
    template <class HandlerType, bool Send, class Handler>
    node* enqueue(Handler&& handler, detail::channel_wait_list<T>& list) {
        using op = detail::channel_op<T, Executor, HandlerType, Send>;
        cancellation_slot slot = get_associated_cancellation_slot(handler);
        op* const n = op::create(ex_, std::forward<Handler>(handler));
        if (slot.is_connected()) {
            n->add_ref();
            try {
                slot.template emplace<cancel_handler>(this, n);
            } catch (...) {
                n->release(); // emplace 失败时处理器没有构造，补回它那份引用
                n->discard();
                n->release();
                throw;
            }
        }
        n->queued.store(true, std::memory_order_release);
        list.push(n);
        return n;
    }

    void cancel_node(node* n) {
        lock_type lock{mutex_};
        if (!n->queued.load(std::memory_order_relaxed)) return;
        (n->is_send ? senders_ : receivers_).erase(n);
        n->queued.store(false, std::memory_order_release);
        n->ec = error::make_error_code(error::operation_aborted);
        n->value.reset(); // 被取消的发送不入队；接收方本来就没有值
        lock.unlock();
        finish(n);
    }

    void detach_all(detail::channel_wait_list<T>& list, detail::channel_wait_list<T>& done, channel_errc e) {
        while (!list.empty()) {
            node* const n = list.pop();
            n->queued.store(false, std::memory_order_release);
            n->ec = make_error_code(e);
            done.push(n);
        }
    }

    void finish_all(detail::channel_wait_list<T>& done) {
        while (!done.empty()) {
            node* const n = done.pop();
            n->value.reset(); // 关闭 / 取消时等待中的发送值被丢弃
            finish(n);
        }
    }

    // 锁外：投递完成，释放队列持有的那份引用。
    static void finish(node* n) {
        if (n == nullptr) return;
        struct release_guard {
            node* n;
            ~release_guard() { n->release(); }
        } guard{n};
        n->complete();
    }

    void handle_dropped(aw::optional<T>& discarded) noexcept {
        if (!discarded) return;
        if (on_drop_) on_drop_(std::move(*discarded)); // noexcept：抛出即 terminate
        discarded.reset();
    }

    static void discard_all(detail::channel_wait_list<T>& list) noexcept {
        while (!list.empty()) {
            node* const n = list.pop();
            n->queued.store(false, std::memory_order_release);
            n->discard();
            n->release();
        }
    }

    void push_back(T&& value) {
        std::size_t index = head_ + count_;
        if (index >= capacity_) index -= capacity_;
        ring_[index].emplace(std::move(value));
        ++count_;
    }
    T pop_front() {
        T value(std::move(*ring_[head_]));
        ring_[head_].reset();
        if (++head_ == capacity_) head_ = 0U;
        --count_;
        return value;
    }

    mutable std::mutex mutex_;
    Executor ex_;
    std::size_t capacity_;
    channel_policy policy_;
    std::vector<aw::optional<T>> ring_;
    std::size_t head_{0U};
    std::size_t count_{0U};
    bool open_{true};
    std::uint64_t dropped_{0U};
    detail::channel_wait_list<T> receivers_;
    detail::channel_wait_list<T> senders_;
    co2::detail::MoveOnlyFunction<void(T&&)> on_drop_;
};

} // namespace aw
