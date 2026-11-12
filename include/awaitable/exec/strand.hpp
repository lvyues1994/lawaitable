#pragma once

#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/operation.hpp"

namespace aw {
namespace detail {

// 侵入式 FIFO（operation::next_）。
struct op_queue {
    operation* front{nullptr};
    operation* back{nullptr};

    bool empty() const noexcept { return front == nullptr; }
    void push(operation* op) noexcept {
        op->next_ = nullptr;
        if (back != nullptr) {
            back->next_ = op;
        } else {
            front = op;
        }
        back = op;
    }
    operation* pop() noexcept {
        operation* const op = front;
        front = op->next_;
        if (front == nullptr) back = nullptr;
        op->next_ = nullptr;
        return op;
    }
    void splice(op_queue& other) noexcept {
        if (other.empty()) return;
        if (back != nullptr) {
            back->next_ = other.front;
        } else {
            front = other.front;
        }
        back = other.back;
        other.front = other.back = nullptr;
    }
    void destroy_all() noexcept {
        while (!empty()) pop()->destroy();
    }
};

// 照搬 asio strand_executor_service::strand_impl：locked 为真表示有一个 invoker 已投递或正在运行；
// ready 只由持有 locked 的 invoker 访问，waiting 由互斥锁保护。
struct strand_impl {
    std::mutex mutex;
    bool locked{false};
    op_queue waiting;
    op_queue ready;

    strand_impl() = default;
    strand_impl(strand_impl const&) = delete;
    strand_impl& operator=(strand_impl const&) = delete;
    ~strand_impl() {
        waiting.destroy_all();
        ready.destroy_all();
    }

    // 线程局部的 strand 调用栈，供 running_in_this_thread。
    struct call_stack_entry {
        strand_impl const* impl;
        call_stack_entry* next;
    };
    static call_stack_entry*& top() noexcept {
        static thread_local call_stack_entry* current = nullptr;
        return current;
    }
    static bool running_in_this_thread(strand_impl const* impl) noexcept {
        for (call_stack_entry* e = top(); e != nullptr; e = e->next)
            if (e->impl == impl) return true;
        return false;
    }

    // 入队。返回 true 表示调用方需要投递 invoker。
    bool enqueue(operation* op) noexcept {
        std::lock_guard<std::mutex> lock{mutex};
        if (locked) {
            waiting.push(op);
            return false;
        }
        locked = true;
        ready.push(op);
        return true;
    }
};

template <class Executor> struct strand_invoker {
    std::shared_ptr<strand_impl> impl;
    Executor inner;

    // 收尾：把 waiting 移进 ready；ready 非空就重新投递 invoker，否则解锁。异常离开时同样执行。
    struct on_exit {
        strand_invoker* self;
        explicit on_exit(strand_invoker* s) noexcept : self{s} {}
        on_exit(on_exit const&) = delete;
        on_exit& operator=(on_exit const&) = delete;
        ~on_exit() {
            bool more = false;
            {
                std::lock_guard<std::mutex> lock{self->impl->mutex};
                self->impl->ready.splice(self->impl->waiting);
                more = !self->impl->ready.empty();
                if (!more) self->impl->locked = false;
            }
            if (more) self->inner.post(strand_invoker{self->impl, self->inner});
        }
    };

    void operator()() {
        strand_impl::call_stack_entry entry{impl.get(), strand_impl::top()};
        strand_impl::top() = &entry;
        struct pop_entry {
            strand_impl::call_stack_entry* entry;
            ~pop_entry() { strand_impl::top() = entry->next; }
        } pop{&entry};
        on_exit exit_guard{this};
        while (!impl->ready.empty()) impl->ready.pop()->complete();
    }
};

} // namespace detail

template <class Executor> class strand {
  public:
    using inner_executor_type = Executor;

    strand() noexcept = default;
    explicit strand(Executor inner)
        : inner_{std::move(inner)}, impl_{std::make_shared<detail::strand_impl>()} {}

    Executor const& get_inner_executor() const noexcept { return inner_; }
    auto& context() const noexcept { return inner_.context(); }

    template <class F> void post(F&& f) const {
        enqueue(detail::make_function_operation(std::forward<F>(f)));
    }

    template <class F> void dispatch(F&& f) const {
        if (running_in_this_thread()) {
            typename std::decay<F>::type function(std::forward<F>(f));
            function();
            return;
        }
        post(std::forward<F>(f));
    }

    bool running_in_this_thread() const noexcept {
        return detail::strand_impl::running_in_this_thread(impl_.get());
    }

    void schedule(co2::coroutine_handle<> coroutine) const noexcept {
        post([coroutine] { coroutine.resume(); });
    }

    friend bool operator==(strand const& a, strand const& b) noexcept { return a.impl_ == b.impl_; }
    friend bool operator!=(strand const& a, strand const& b) noexcept { return !(a == b); }

  private:
    void enqueue(detail::operation* op) const {
        if (impl_->enqueue(op)) inner_.post(detail::strand_invoker<Executor>{impl_, inner_});
    }

    Executor inner_{};
    std::shared_ptr<detail::strand_impl> impl_;
};

template <class Executor> strand<Executor> make_strand(Executor const& ex) {
    return strand<Executor>{ex};
}

} // namespace aw
