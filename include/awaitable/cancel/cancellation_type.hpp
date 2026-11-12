#pragma once

namespace aw {

enum class cancellation_type : unsigned {
    none = 0U,
    terminal = 1U,
    partial = 2U,
    total = 4U,
    all = 0xFFFFFFFFU
};

constexpr cancellation_type operator|(cancellation_type a, cancellation_type b) noexcept {
    return static_cast<cancellation_type>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr cancellation_type operator&(cancellation_type a, cancellation_type b) noexcept {
    return static_cast<cancellation_type>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}
constexpr cancellation_type operator^(cancellation_type a, cancellation_type b) noexcept {
    return static_cast<cancellation_type>(static_cast<unsigned>(a) ^ static_cast<unsigned>(b));
}
constexpr cancellation_type operator~(cancellation_type a) noexcept {
    return static_cast<cancellation_type>(~static_cast<unsigned>(a));
}
constexpr bool operator!(cancellation_type a) noexcept { return static_cast<unsigned>(a) == 0U; }
inline cancellation_type& operator|=(cancellation_type& a, cancellation_type b) noexcept {
    return a = a | b;
}
inline cancellation_type& operator&=(cancellation_type& a, cancellation_type b) noexcept {
    return a = a & b;
}
inline cancellation_type& operator^=(cancellation_type& a, cancellation_type b) noexcept {
    return a = a ^ b;
}

} // namespace aw
