#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/cancel/cancellation_signal.hpp"

namespace aw {

// ---- associated_executor ----

template <class T, class Default, class = void> struct associated_executor_impl {
    using type = Default;
    static type get(T const&, Default const& d) noexcept { return d; }
};
template <class T, class Default>
struct associated_executor_impl<T, Default,
                                decltype(static_cast<void>(std::declval<T const&>().get_executor()),
                                         static_cast<void>(sizeof(typename T::executor_type)))> {
    using type = typename T::executor_type;
    static type get(T const& t, Default const&) noexcept { return t.get_executor(); }
};

template <class T, class Default> struct associated_executor : associated_executor_impl<T, Default> {};

template <class T, class Default>
typename associated_executor<T, Default>::type get_associated_executor(T const& t, Default const& d) {
    return associated_executor<T, Default>::get(t, d);
}

// ---- associated_cancellation_slot ----

template <class T, class = void> struct associated_cancellation_slot_impl {
    static cancellation_slot get(T const&) noexcept { return cancellation_slot{}; }
};
template <class T>
struct associated_cancellation_slot_impl<
    T, decltype(static_cast<void>(std::declval<T const&>().get_cancellation_slot()))> {
    static cancellation_slot get(T const& t) noexcept { return t.get_cancellation_slot(); }
};

template <class T> struct associated_cancellation_slot : associated_cancellation_slot_impl<T> {};

template <class T> cancellation_slot get_associated_cancellation_slot(T const& t) {
    return associated_cancellation_slot<T>::get(t);
}

// ---- 绑定器 ----

template <class Handler, class Executor> class executor_binder {
  public:
    using executor_type = Executor;
    template <class H>
    executor_binder(Executor const& ex, H&& h) : executor_{ex}, handler_(std::forward<H>(h)) {}
    executor_type get_executor() const noexcept { return executor_; }
    Handler const& get() const noexcept { return handler_; }
    template <class... Args> void operator()(Args&&... args) { handler_(std::forward<Args>(args)...); }

  private:
    Executor executor_;
    Handler handler_;
};

template <class Executor, class H> executor_binder<typename std::decay<H>::type, Executor>
bind_executor(Executor const& ex, H&& h) {
    return executor_binder<typename std::decay<H>::type, Executor>{ex, std::forward<H>(h)};
}

template <class Handler> class cancellation_slot_binder {
  public:
    template <class H>
    cancellation_slot_binder(cancellation_slot slot, H&& h) : slot_{slot}, handler_(std::forward<H>(h)) {}
    cancellation_slot get_cancellation_slot() const noexcept { return slot_; }
    Handler const& get() const noexcept { return handler_; }
    template <class... Args> void operator()(Args&&... args) { handler_(std::forward<Args>(args)...); }

  private:
    cancellation_slot slot_;
    Handler handler_;
};

template <class H> cancellation_slot_binder<typename std::decay<H>::type>
bind_cancellation_slot(cancellation_slot slot, H&& h) {
    return cancellation_slot_binder<typename std::decay<H>::type>{slot, std::forward<H>(h)};
}

// 绑定器转发另一种关联。
template <class Handler, class Executor>
struct associated_cancellation_slot<executor_binder<Handler, Executor>> {
    static cancellation_slot get(executor_binder<Handler, Executor> const& b) noexcept {
        return get_associated_cancellation_slot(b.get());
    }
};
template <class Handler, class Default>
struct associated_executor<cancellation_slot_binder<Handler>, Default> {
    using type = typename associated_executor<Handler, Default>::type;
    static type get(cancellation_slot_binder<Handler> const& b, Default const& d) noexcept {
        return associated_executor<Handler, Default>::get(b.get(), d);
    }
};

} // namespace aw
