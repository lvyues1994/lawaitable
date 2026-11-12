#pragma once

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"

namespace aw {

template <cancellation_type Mask> struct cancellation_filter {
    cancellation_type operator()(cancellation_type type) const noexcept { return type & Mask; }
};

using disable_cancellation = cancellation_filter<cancellation_type::none>;
using enable_terminal_cancellation = cancellation_filter<cancellation_type::terminal>;
using enable_partial_cancellation =
    cancellation_filter<cancellation_type::terminal | cancellation_type::partial>;
using enable_total_cancellation =
    cancellation_filter<cancellation_type::terminal | cancellation_type::partial |
                        cancellation_type::total>;

// asio cancellation_state.hpp:31-197。impl 作为处理器住在父 slot 里，state 只是指向它的指针。
class cancellation_state {
  public:
    constexpr cancellation_state() noexcept = default;

    template <class In = enable_terminal_cancellation, class Out = In>
    explicit cancellation_state(cancellation_slot parent, In in = In{}, Out out = Out{})
        : impl_{parent.is_connected() ? &parent.template emplace<impl<In, Out>>(in, out) : nullptr} {}

    cancellation_slot slot() const noexcept {
        return impl_ != nullptr ? impl_->signal_.slot() : cancellation_slot{};
    }

    cancellation_type cancelled() const noexcept {
        return impl_ != nullptr ? impl_->cancelled_ : cancellation_type::none;
    }

    void clear(cancellation_type mask = cancellation_type::all) noexcept {
        if (impl_ != nullptr) impl_->cancelled_ &= ~mask;
    }

    void set_observer(void (*fn)(void*, cancellation_type), void* arg) noexcept {
        if (impl_ != nullptr) {
            impl_->observer_ = fn;
            impl_->observer_arg_ = arg;
        }
    }

  private:
    struct impl_base {
        impl_base() = default;
        impl_base(impl_base const&) = delete;
        impl_base& operator=(impl_base const&) = delete;
        cancellation_signal signal_;
        cancellation_type cancelled_{cancellation_type::none};
        void (*observer_)(void*, cancellation_type){nullptr};
        void* observer_arg_{nullptr};
    };

    template <class In, class Out> struct impl : impl_base {
        impl(In in, Out out) : in_{in}, out_{out} {}
        void operator()(cancellation_type type) {
            this->cancelled_ = in_(type);
            cancellation_type const forwarded = out_(this->cancelled_);
            if (!!this->cancelled_ && this->observer_ != nullptr)
                this->observer_(this->observer_arg_, this->cancelled_);
            if (!!forwarded) this->signal_.emit(forwarded);
        }
        In in_;
        Out out_;
    };

    impl_base* impl_{nullptr};
};

} // namespace aw
