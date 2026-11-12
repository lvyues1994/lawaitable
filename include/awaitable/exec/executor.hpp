#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/operation.hpp"

namespace aw {

// §5.1 的 executor：保存 Context*，对 final 的具体类非虚调用。默认构造的句柄无效，只能被赋值。
template <class Context> class basic_executor {
  public:
    basic_executor() noexcept = default;
    explicit basic_executor(Context& context) noexcept : context_{&context} {}

    Context& context() const noexcept { return *context_; }

    template <class F> void post(F&& f) const {
        context_->post(detail::make_function_operation(std::forward<F>(f)));
    }

    template <class F> void dispatch(F&& f) const {
        if (context_->running_in_this_thread()) {
            typename std::decay<F>::type function(std::forward<F>(f));
            function();
            return;
        }
        post(std::forward<F>(f));
    }

    bool running_in_this_thread() const noexcept { return context_->running_in_this_thread(); }

    void schedule(co2::coroutine_handle<> coroutine) const noexcept { context_->schedule(coroutine); }

    friend bool operator==(basic_executor const& a, basic_executor const& b) noexcept {
        return a.context_ == b.context_;
    }
    friend bool operator!=(basic_executor const& a, basic_executor const& b) noexcept {
        return !(a == b);
    }

  private:
    Context* context_{nullptr};
};

template <class T, class = void> struct is_executor : std::false_type {};
template <class T>
struct is_executor<T, decltype(static_cast<void>(std::declval<T const&>().context()),
                               static_cast<void>(std::declval<T const&>().running_in_this_thread()))>
    : std::true_type {};

} // namespace aw
