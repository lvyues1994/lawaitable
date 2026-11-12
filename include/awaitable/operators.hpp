#pragma once

#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/awaitable.hpp"
#include "awaitable/core/dsl.hpp"
#include "awaitable/impl/parallel_group.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/variant.hpp"

// §12 第 25 行：a && b 是 wait_for_one_error，a || b 是 wait_for_one_success（asio
// experimental/awaitable_operators.hpp:75-300）。用法：using namespace aw::awaitable_operators;
namespace aw {
namespace detail {

// ---- && 的结果类型：void 消失；左边是 tuple 时展平 ----
template <class T, class U> struct and_join {
    using type = std::tuple<T, U>;
    template <class O> static type make(O& o) { return type(unwrap_child(o.v0), unwrap_child(o.v1)); }
};
template <> struct and_join<void, void> {
    using type = void;
};
template <class T> struct and_join<T, void> {
    using type = T;
    template <class O> static type make(O& o) { return unwrap_child(o.v0); }
};
template <class U> struct and_join<void, U> {
    using type = U;
    template <class O> static type make(O& o) { return unwrap_child(o.v1); }
};
template <class... Ts> struct and_join<std::tuple<Ts...>, void> {
    using type = std::tuple<Ts...>;
    template <class O> static type make(O& o) { return unwrap_child(o.v0); }
};
template <class... Ts, class U> struct and_join<std::tuple<Ts...>, U> {
    using type = std::tuple<Ts..., U>;
    template <class O> static type make(O& o) {
        return std::tuple_cat(unwrap_child(o.v0), std::tuple<U>(unwrap_child(o.v1)));
    }
};

// ---- || 的结果类型：void 变成 monostate；左边是 variant 时展平 ----
template <class T> struct or_alt { using type = T; };
template <> struct or_alt<void> { using type = aw::monostate; };

template <class R, std::size_t Offset, class O> R or_take_right(O&, std::true_type /*void*/) {
    return R(aw::in_place_index<Offset>());
}
template <class R, std::size_t Offset, class O> R or_take_right(O& o, std::false_type) {
    return R(aw::in_place_index<Offset>(), unwrap_child(o.v1));
}

template <class T, class U> struct or_join {
    using type = aw::variant<typename or_alt<T>::type, typename or_alt<U>::type>;
    template <class O> static type left(O& o) { return left(o, std::is_void<T>{}); }
    template <class O> static type right(O& o) { return or_take_right<type, 1U>(o, std::is_void<U>{}); }

  private:
    template <class O> static type left(O&, std::true_type) { return type(aw::in_place_index<0U>()); }
    template <class O> static type left(O& o, std::false_type) {
        return type(aw::in_place_index<0U>(), unwrap_child(o.v0));
    }
};

template <class R, class Src> struct variant_rewrap;
template <class R, class... Ts> struct variant_rewrap<R, aw::variant<Ts...>> {
    template <std::size_t I> static R go(aw::variant<Ts...>& v, std::integral_constant<std::size_t, I>) {
        if (v.index() == I) return R(aw::in_place_index<I>(), aw::get<I>(std::move(v)));
        return go(v, std::integral_constant<std::size_t, I + 1U>{});
    }
    static R go(aw::variant<Ts...>& v, std::integral_constant<std::size_t, sizeof...(Ts) - 1U>) {
        return R(aw::in_place_index<sizeof...(Ts) - 1U>(), aw::get<sizeof...(Ts) - 1U>(std::move(v)));
    }
};

template <class... Ts, class U> struct or_join<aw::variant<Ts...>, U> {
    using type = aw::variant<Ts..., typename or_alt<U>::type>;
    template <class O> static type left(O& o) {
        return variant_rewrap<type, aw::variant<Ts...>>::go(unwrap_child_ref(o.v0),
                                                            std::integral_constant<std::size_t, 0U>{});
    }
    template <class O> static type right(O& o) {
        return or_take_right<type, sizeof...(Ts)>(o, std::is_void<U>{});
    }

  private:
    template <class V> static V& unwrap_child_ref(aw::optional<V>& v) { return *v; }
    template <class V> static V& unwrap_child_ref(aw::optional<aw::optional<V>>& v) { return **v; }
};

inline void rethrow_and(std::exception_ptr const& e0, std::exception_ptr const& e1) {
    if (e0 && e1) throw multiple_exceptions(e0);
    if (e0) std::rethrow_exception(e0);
    if (e1) std::rethrow_exception(e1);
}

template <class T, class U, class Executor>
auto and_impl(awaitable<T, Executor> t, awaitable<U, Executor> u)
    CO2_BEG((awaitable<typename and_join<T, U>::type, Executor>), (t, u), pair_outcome<T, U> out{};) {
    CO2_AWAIT_SET(out, async_pair_group(group_condition::one_error, std::move(t), std::move(u), use_awaitable));
    rethrow_and(out.e0, out.e1);
    CO2_RETURN(and_join<T, U>::make(out));
}
CO2_END

template <class T, class U, class Executor>
auto and_void_impl(awaitable<T, Executor> t, awaitable<U, Executor> u)
    CO2_BEG((awaitable<void, Executor>), (t, u), pair_outcome<T, U> out{};) {
    CO2_AWAIT_SET(out, async_pair_group(group_condition::one_error, std::move(t), std::move(u), use_awaitable));
    rethrow_and(out.e0, out.e1);
    CO2_RETURN();
}
CO2_END

// asio：先看先完成者；它失败再看另一个；都失败抛 multiple_exceptions(先完成者的异常)。
template <class T, class U, class Executor>
auto or_impl(awaitable<T, Executor> t, awaitable<U, Executor> u)
    CO2_BEG((awaitable<typename or_join<T, U>::type, Executor>), (t, u), pair_outcome<T, U> out{};) {
    CO2_AWAIT_SET(out, async_pair_group(group_condition::one_success, std::move(t), std::move(u), use_awaitable));
    if (out.first == 0U) {
        if (!out.e0) CO2_RETURN(or_join<T, U>::left(out));
        if (!out.e1) CO2_RETURN(or_join<T, U>::right(out));
        throw multiple_exceptions(out.e0);
    }
    if (!out.e1) CO2_RETURN(or_join<T, U>::right(out));
    if (!out.e0) CO2_RETURN(or_join<T, U>::left(out));
    throw multiple_exceptions(out.e1);
}
CO2_END

template <class T, class U> struct and_dispatch {
    template <class E> static auto run(awaitable<T, E> t, awaitable<U, E> u) {
        return and_impl(std::move(t), std::move(u));
    }
};
template <> struct and_dispatch<void, void> {
    template <class E> static auto run(awaitable<void, E> t, awaitable<void, E> u) {
        return and_void_impl(std::move(t), std::move(u));
    }
};

} // namespace detail

namespace awaitable_operators {

template <class T, class U, class Executor>
awaitable<typename detail::and_join<T, U>::type, Executor> operator&&(awaitable<T, Executor> t,
                                                                     awaitable<U, Executor> u) {
    return detail::and_dispatch<T, U>::run(std::move(t), std::move(u));
}

template <class T, class U, class Executor>
awaitable<typename detail::or_join<T, U>::type, Executor> operator||(awaitable<T, Executor> t,
                                                                    awaitable<U, Executor> u) {
    return detail::or_impl(std::move(t), std::move(u));
}

} // namespace awaitable_operators
} // namespace aw
