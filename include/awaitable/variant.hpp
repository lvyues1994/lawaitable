#pragma once

#include <cstddef>

#include "awaitable/core/config.hpp"

#if CO2_CPLUSPLUS >= 201703L
#include <variant>
namespace aw {
using std::get;
using std::get_if;
using std::in_place_index_t;
using std::monostate;
using std::variant;
template <std::size_t I> constexpr std::in_place_index_t<I> in_place_index() noexcept {
    return std::in_place_index_t<I>{};
}
} // namespace aw
#else
#include <algorithm>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aw {

struct monostate {};

template <std::size_t I> struct in_place_index_t {
    explicit constexpr in_place_index_t() noexcept = default;
};
template <std::size_t I> constexpr in_place_index_t<I> in_place_index() noexcept {
    return in_place_index_t<I>{};
}

struct bad_variant_access : std::logic_error {
    bad_variant_access() : std::logic_error{"bad variant access"} {}
};

namespace detail {

template <std::size_t I, class... Ts> struct nth_type;
template <class T, class... Ts> struct nth_type<0, T, Ts...> { using type = T; };
template <std::size_t I, class T, class... Ts> struct nth_type<I, T, Ts...> {
    using type = typename nth_type<I - 1, Ts...>::type;
};

template <std::size_t A, std::size_t... Rest> struct max_of {
    static constexpr std::size_t value = A > max_of<Rest...>::value ? A : max_of<Rest...>::value;
};
template <std::size_t A> struct max_of<A> { static constexpr std::size_t value = A; };

template <class... Ts> struct variant_ops;
template <> struct variant_ops<> {
    static void destroy(std::size_t, void*) noexcept {}
    static void move(std::size_t, void*, void*) {}
};
template <class T, class... Ts> struct variant_ops<T, Ts...> {
    static void destroy(std::size_t index, void* p) noexcept {
        if (index == 0U) {
            static_cast<T*>(p)->~T();
        } else {
            variant_ops<Ts...>::destroy(index - 1U, p);
        }
    }
    static void move(std::size_t index, void* to, void* from) {
        if (index == 0U) {
            ::new (to) T(std::move(*static_cast<T*>(from)));
        } else {
            variant_ops<Ts...>::move(index - 1U, to, from);
        }
    }
};

} // namespace detail

template <class... Ts> class variant {
    static_assert(sizeof...(Ts) > 0U, "aw::variant needs at least one alternative");
    using first_type = typename detail::nth_type<0, Ts...>::type;

  public:
    variant() : index_{0U} { ::new (static_cast<void*>(storage_)) first_type(); }

    template <std::size_t I, class... Args>
    explicit variant(in_place_index_t<I>, Args&&... args) : index_{I} {
        using alternative = typename detail::nth_type<I, Ts...>::type;
        ::new (static_cast<void*>(storage_)) alternative(std::forward<Args>(args)...);
    }

    variant(variant&& other) : index_{other.index_} {
        detail::variant_ops<Ts...>::move(index_, storage_, other.storage_);
    }
    variant(variant const&) = delete;
    variant& operator=(variant const&) = delete;
    variant& operator=(variant&& other) {
        if (this != &other) {
            detail::variant_ops<Ts...>::destroy(index_, storage_);
            index_ = other.index_;
            detail::variant_ops<Ts...>::move(index_, storage_, other.storage_);
        }
        return *this;
    }
    ~variant() { detail::variant_ops<Ts...>::destroy(index_, storage_); }

    std::size_t index() const noexcept { return index_; }

    template <std::size_t I> typename detail::nth_type<I, Ts...>::type* get_ptr() noexcept {
        using alternative = typename detail::nth_type<I, Ts...>::type;
        return index_ == I ? reinterpret_cast<alternative*>(storage_) : nullptr;
    }

  private:
    alignas(detail::max_of<alignof(Ts)...>::value) unsigned char
        storage_[detail::max_of<sizeof(Ts)...>::value];
    std::size_t index_;
};

template <std::size_t I, class... Ts>
typename detail::nth_type<I, Ts...>::type& get(variant<Ts...>& v) {
    auto* p = v.template get_ptr<I>();
    if (p == nullptr) throw bad_variant_access{};
    return *p;
}

template <std::size_t I, class... Ts>
typename detail::nth_type<I, Ts...>::type&& get(variant<Ts...>&& v) {
    return std::move(get<I>(v));
}

template <std::size_t I, class... Ts>
typename detail::nth_type<I, Ts...>::type* get_if(variant<Ts...>* v) noexcept {
    return v == nullptr ? nullptr : v->template get_ptr<I>();
}

} // namespace aw
#endif
