#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

// channel 规格增补 C2：可默认构造的 T 原样交付，否则交付 aw::optional<T>（同主规格 D8）。
template <class T>
using channel_receive_type =
    typename std::conditional<std::is_default_constructible<T>::value, T, aw::optional<T>>::type;

template <class T> T channel_take(aw::optional<T>& v, std::true_type /* 交付 T */) {
    if (v) return T(std::move(*v));
    return T();
}
template <class T> aw::optional<T> channel_take(aw::optional<T>& v, std::false_type /* 交付 optional */) {
    if (v) return aw::optional<T>(std::move(*v));
    return aw::optional<T>();
}
template <class T> channel_receive_type<T> channel_take(aw::optional<T>& v) {
    return channel_take(v, std::integral_constant<bool, std::is_default_constructible<T>::value>{});
}

// 投递出去的完成：回调 + 错误码（+ 接收到的值）。
template <class Handler> struct channel_send_completion {
    Handler handler;
    std::error_code ec;
    void operator()() { handler(ec); }
};

template <class Handler, class T> struct channel_receive_completion {
    Handler handler;
    std::error_code ec;
    aw::optional<T> value;
    void operator()() { handler(ec, channel_take(value)); }
};

template <class Executor, class Handler>
void channel_post_send(Executor const& fallback, Handler&& handler, std::error_code ec) {
    using handler_type = typename std::decay<Handler>::type;
    auto target = get_associated_executor(handler, fallback);
    target.post(channel_send_completion<handler_type>{std::forward<Handler>(handler), ec});
}

template <class T, class Executor, class Handler>
void channel_post_receive(Executor const& fallback, Handler&& handler, std::error_code ec, aw::optional<T> value) {
    using handler_type = typename std::decay<Handler>::type;
    auto target = get_associated_executor(handler, fallback);
    target.post(channel_receive_completion<handler_type, T>{std::forward<Handler>(handler), ec, std::move(value)});
}

// 等待节点（C6）。prev / next / value / ec 只在 channel 的锁内修改；queued 也只在锁内写，
// slot 处理器在锁外读它，已出队就不再碰 channel。节点按引用计数释放：队列一份，slot 处理器一份。
template <class T> struct channel_node {
    struct vtable {
        void (*complete)(channel_node*); // 移出回调并投递；只调用一次
        void (*discard)(channel_node*) noexcept; // 销毁回调而不调用
        void (*free)(channel_node*) noexcept;
    };

    explicit channel_node(vtable const* table, bool sender) noexcept : vt{table}, is_send{sender} {}
    channel_node(channel_node const&) = delete;
    channel_node& operator=(channel_node const&) = delete;

    void complete() { vt->complete(this); }
    void discard() noexcept { vt->discard(this); }
    void add_ref() noexcept { refs.fetch_add(1U, std::memory_order_relaxed); }
    void release() noexcept {
        if (refs.fetch_sub(1U, std::memory_order_acq_rel) == 1U) vt->free(this);
    }

    vtable const* vt;
    channel_node* prev{nullptr};
    channel_node* next{nullptr};
    std::atomic<bool> queued{false};
    std::atomic<unsigned> refs{1U};
    bool is_send;
    std::error_code ec{};
    aw::optional<T> value{}; // 发送方：要发送的值；接收方：交付的值
};

template <class T, class Executor, class Handler, bool Send> struct channel_op final : channel_node<T> {
    using base = channel_node<T>;

    template <class H>
    channel_op(Executor const& ex, H&& h) : base{table(), Send}, fallback{ex}, handler(std::forward<H>(h)) {}

    static void do_complete(base* b) {
        auto* const self = static_cast<channel_op*>(b);
        Handler h(std::move(*self->handler));
        self->handler.reset();
        post(self, std::move(h), std::integral_constant<bool, Send>{});
    }
    static void post(channel_op* self, Handler&& h, std::true_type) {
        channel_post_send(self->fallback, std::move(h), self->ec);
    }
    static void post(channel_op* self, Handler&& h, std::false_type) {
        aw::optional<T> v(std::move(self->value));
        self->value.reset();
        channel_post_receive<T>(self->fallback, std::move(h), self->ec, std::move(v));
    }
    static void do_discard(base* b) noexcept { static_cast<channel_op*>(b)->handler.reset(); }
    static void do_free(base* b) noexcept {
        auto* const self = static_cast<channel_op*>(b);
        self->~channel_op();
        recycling_deallocate(mem_tag::channel_op, self, sizeof(channel_op));
    }

    static typename base::vtable const* table() noexcept {
        static constexpr typename base::vtable instance{&do_complete, &do_discard, &do_free};
        return &instance;
    }

    template <class H> static channel_op* create(Executor const& ex, H&& h) {
        static_assert(alignof(channel_op) <= alignof(std::max_align_t), "channel: over-aligned handler");
        void* const memory = recycling_allocate(mem_tag::channel_op, sizeof(channel_op), alignof(channel_op));
        try {
            return ::new (memory) channel_op(ex, std::forward<H>(h));
        } catch (...) {
            recycling_deallocate(mem_tag::channel_op, memory, sizeof(channel_op));
            throw;
        }
    }

    Executor fallback;
    aw::optional<Handler> handler;
};

// 侵入式 FIFO，支持从中间摘除（取消）。
template <class T> struct channel_wait_list {
    using node = channel_node<T>;
    node* front{nullptr};
    node* back{nullptr};

    bool empty() const noexcept { return front == nullptr; }
    void push(node* n) noexcept {
        n->prev = back;
        n->next = nullptr;
        if (back != nullptr) {
            back->next = n;
        } else {
            front = n;
        }
        back = n;
    }
    void erase(node* n) noexcept {
        if (n->prev != nullptr) {
            n->prev->next = n->next;
        } else {
            front = n->next;
        }
        if (n->next != nullptr) {
            n->next->prev = n->prev;
        } else {
            back = n->prev;
        }
        n->prev = n->next = nullptr;
    }
    node* pop() noexcept {
        node* const n = front;
        erase(n);
        return n;
    }
};

} // namespace detail
} // namespace aw
