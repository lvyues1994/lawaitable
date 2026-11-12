#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"

namespace aw {
namespace detail {

class cancellation_handler_base {
  public:
    virtual void call(cancellation_type type) = 0;
    // 析构自身，返回所占内存（地址与分配时的大小），由调用方决定复用还是归还。
    virtual std::pair<void*, std::size_t> destroy() noexcept = 0;

  protected:
    virtual ~cancellation_handler_base() = default;
};

template <class Handler> class cancellation_handler final : public cancellation_handler_base {
  public:
    template <class... Args>
    explicit cancellation_handler(std::size_t size, Args&&... args)
        : handler_(std::forward<Args>(args)...), size_{size} {}

    void call(cancellation_type type) override { handler_(type); }

    std::pair<void*, std::size_t> destroy() noexcept override {
        std::pair<void*, std::size_t> const memory{this, size_};
        this->~cancellation_handler();
        return memory;
    }

    Handler& handler() noexcept { return handler_; }

  private:
    ~cancellation_handler() override = default;

    Handler handler_;
    std::size_t size_;
};

} // namespace detail

class cancellation_slot;

class cancellation_signal {
  public:
    cancellation_signal() noexcept = default;
    cancellation_signal(cancellation_signal const&) = delete;
    cancellation_signal& operator=(cancellation_signal const&) = delete;
    ~cancellation_signal() {
        if (handler_ != nullptr) {
            std::pair<void*, std::size_t> const memory = handler_->destroy();
            detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                         memory.second);
        }
    }

    void emit(cancellation_type type) {
        if (handler_ != nullptr) handler_->call(type);
    }

    cancellation_slot slot() noexcept;

  private:
    detail::cancellation_handler_base* handler_{nullptr};
};

class cancellation_slot {
  public:
    constexpr cancellation_slot() noexcept = default;

    template <class H, class... Args> H& emplace(Args&&... args) {
        using handler_type = detail::cancellation_handler<H>;
        std::pair<void*, std::size_t> const memory =
            prepare_memory(sizeof(handler_type), alignof(handler_type));
        handler_type* handler = nullptr;
        try {
            handler = ::new (memory.first) handler_type(memory.second, std::forward<Args>(args)...);
        } catch (...) {
            detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                         memory.second);
            throw;
        }
        *handler_ = handler;
        return handler->handler();
    }

    template <class H> typename std::decay<H>::type& assign(H&& handler) {
        return emplace<typename std::decay<H>::type>(std::forward<H>(handler));
    }

    void clear() {
        if (handler_ != nullptr && *handler_ != nullptr) {
            std::pair<void*, std::size_t> const memory = (*handler_)->destroy();
            *handler_ = nullptr;
            detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                         memory.second);
        }
    }

    bool is_connected() const noexcept { return handler_ != nullptr; }
    bool has_handler() const noexcept { return handler_ != nullptr && *handler_ != nullptr; }

    friend bool operator==(cancellation_slot const& a, cancellation_slot const& b) noexcept {
        return a.handler_ == b.handler_;
    }
    friend bool operator!=(cancellation_slot const& a, cancellation_slot const& b) noexcept {
        return !(a == b);
    }

  private:
    friend class cancellation_signal;
    explicit cancellation_slot(detail::cancellation_handler_base** handler) noexcept
        : handler_{handler} {}

    // 旧处理器的块够大、对齐合适就复用（asio cancellation_signal.hpp:150-198）。
    std::pair<void*, std::size_t> prepare_memory(std::size_t size, std::size_t align) {
        std::pair<void*, std::size_t> memory{nullptr, 0U};
        if (*handler_ != nullptr) {
            memory = (*handler_)->destroy();
            *handler_ = nullptr;
        }
        if (memory.first == nullptr || size > memory.second ||
            reinterpret_cast<std::size_t>(memory.first) % align != 0U) {
            if (memory.first != nullptr)
                detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                             memory.second);
            memory.first =
                detail::recycling_allocate(detail::mem_tag::cancellation_signal, size, align);
            memory.second = size;
        }
        return memory;
    }

    detail::cancellation_handler_base** handler_{nullptr};
};

inline cancellation_slot cancellation_signal::slot() noexcept { return cancellation_slot{&handler_}; }

} // namespace aw
