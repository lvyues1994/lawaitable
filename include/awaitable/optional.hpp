#pragma once

#include "awaitable/core/config.hpp"

#if CO2_CPLUSPLUS >= 201703L
#include <optional>
namespace aw {
using std::nullopt;
using std::nullopt_t;
using std::optional;
} // namespace aw
#else
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aw {

struct nullopt_t {
    explicit constexpr nullopt_t(int) noexcept {}
};
constexpr nullopt_t nullopt{0};

struct bad_optional_access : std::logic_error {
    bad_optional_access() : std::logic_error{"bad optional access"} {}
};

template <class T> class optional {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value,
                  "aw::optional<T> requires a non-array object type");

  public:
    using value_type = T;

    optional() noexcept {}
    optional(nullopt_t) noexcept {}
    optional(T const& value) { construct(value); }
    optional(T&& value) { construct(std::move(value)); }
    optional(optional const& other) {
        if (other.engaged_) construct(*other);
    }
    optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (other.engaged_) construct(std::move(*other));
    }
    ~optional() { reset(); }

    optional& operator=(nullopt_t) noexcept {
        reset();
        return *this;
    }
    optional& operator=(optional const& other) {
        if (this != &other) {
            reset();
            if (other.engaged_) construct(*other);
        }
        return *this;
    }
    optional& operator=(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this != &other) {
            reset();
            if (other.engaged_) construct(std::move(*other));
        }
        return *this;
    }

    bool has_value() const noexcept { return engaged_; }
    explicit operator bool() const noexcept { return engaged_; }

    T& operator*() & noexcept { return *ptr(); }
    T const& operator*() const& noexcept { return *ptr(); }
    T&& operator*() && noexcept { return std::move(*ptr()); }
    T* operator->() noexcept { return ptr(); }
    T const* operator->() const noexcept { return ptr(); }

    T& value() & {
        if (!engaged_) throw bad_optional_access{};
        return *ptr();
    }
    T const& value() const& {
        if (!engaged_) throw bad_optional_access{};
        return *ptr();
    }
    T&& value() && {
        if (!engaged_) throw bad_optional_access{};
        return std::move(*ptr());
    }

    template <class... Args> T& emplace(Args&&... args) {
        reset();
        construct(std::forward<Args>(args)...);
        return *ptr();
    }

    void reset() noexcept {
        if (engaged_) {
            ptr()->~T();
            engaged_ = false;
        }
    }

  private:
    template <class... Args> void construct(Args&&... args) {
        ::new (static_cast<void*>(storage_)) T(std::forward<Args>(args)...);
        engaged_ = true;
    }
    T* ptr() noexcept { return reinterpret_cast<T*>(storage_); }
    T const* ptr() const noexcept { return reinterpret_cast<T const*>(storage_); }

    alignas(T) unsigned char storage_[sizeof(T)];
    bool engaged_{false};
};

} // namespace aw
#endif
