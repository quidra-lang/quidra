#pragma once

// Typed, indexed field traversal shared by complete IR inspection and identity.
// This is the golden serializer's brace-constructibility/binding scheme. A
// field added to a nonaggregate adapter, or beyond the ladder, fails the build.
#include <cstddef>
#include <type_traits>
#include <utility>

namespace quidra { struct ShapeExtent; }
namespace quidra::ir { struct Binary; }
namespace quidra::ir::fields {
inline constexpr std::size_t max_aggregate_arity = 32;
namespace detail {
template <std::size_t> struct AnyField {
    template <class T> operator T() const&&;
};
template <class T, std::size_t... I>
constexpr bool brace_constructible(std::index_sequence<I...>) {
    return requires { T{AnyField<I>{}...}; };
}
template <class T, std::size_t N>
constexpr std::size_t aggregate_arity_from() {
    if constexpr (N == 0) return 0;
    else if constexpr (brace_constructible<T>(std::make_index_sequence<N>{})) return N;
    else return aggregate_arity_from<T, N - 1>();
}
template <class F, class... T, std::size_t... I>
void visit_fields(F& visitor, std::index_sequence<I...>, T&... values) {
    (visitor(std::integral_constant<std::size_t, I>{}, values), ...);
}
} // namespace detail

template <class T>
constexpr std::size_t aggregate_arity() {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<V, Binary>) return 9;
    else if constexpr (std::is_same_v<V, ShapeExtent>) return 7;
    else {
        static_assert(std::is_aggregate_v<V>, "nonaggregate needs an explicit complete field adapter");
        return detail::aggregate_arity_from<V, max_aggregate_arity + 4>();
    }
}

// Both const and mutable lvalues are supported; every callback gets its
// declaration-order field index and a reference with the original cv type.
template <class T, class F>
void for_each_field(T& value, F&& visitor) {
    constexpr auto n = aggregate_arity<T>();
    static_assert(n <= max_aggregate_arity, "extend the complete field binding ladder");
    if constexpr (n == 0) {
        static_assert(std::is_empty_v<std::remove_cvref_t<T>>, "nonempty aggregate arity was not detected");
        (void)value;
        (void)visitor;
    }
    else if constexpr (n == 1) {
        auto& [f0] = value;
        detail::visit_fields(visitor, std::make_index_sequence<1>{}, f0);
    }
    else if constexpr (n == 2) {
        auto& [f0, f1] = value;
        detail::visit_fields(visitor, std::make_index_sequence<2>{}, f0, f1);
    }
    else if constexpr (n == 3) {
        auto& [f0, f1, f2] = value;
        detail::visit_fields(visitor, std::make_index_sequence<3>{}, f0, f1, f2);
    }
    else if constexpr (n == 4) {
        auto& [f0, f1, f2, f3] = value;
        detail::visit_fields(visitor, std::make_index_sequence<4>{}, f0, f1, f2, f3);
    }
    else if constexpr (n == 5) {
        auto& [f0, f1, f2, f3, f4] = value;
        detail::visit_fields(visitor, std::make_index_sequence<5>{}, f0, f1, f2, f3, f4);
    }
    else if constexpr (n == 6) {
        auto& [f0, f1, f2, f3, f4, f5] = value;
        detail::visit_fields(visitor, std::make_index_sequence<6>{}, f0, f1, f2, f3, f4, f5);
    }
    else if constexpr (n == 7) {
        auto& [f0, f1, f2, f3, f4, f5, f6] = value;
        detail::visit_fields(visitor, std::make_index_sequence<7>{}, f0, f1, f2, f3, f4, f5, f6);
    }
    else if constexpr (n == 8) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7] = value;
        detail::visit_fields(visitor, std::make_index_sequence<8>{}, f0, f1, f2, f3, f4, f5, f6, f7);
    }
    else if constexpr (n == 9) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8] = value;
        detail::visit_fields(visitor, std::make_index_sequence<9>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8);
    }
    else if constexpr (n == 10) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9] = value;
        detail::visit_fields(visitor, std::make_index_sequence<10>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9);
    }
    else if constexpr (n == 11) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = value;
        detail::visit_fields(visitor, std::make_index_sequence<11>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10);
    }
    else if constexpr (n == 12) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11] = value;
        detail::visit_fields(visitor, std::make_index_sequence<12>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11);
    }
    else if constexpr (n == 13) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12] = value;
        detail::visit_fields(visitor, std::make_index_sequence<13>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12);
    }
    else if constexpr (n == 14) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = value;
        detail::visit_fields(visitor, std::make_index_sequence<14>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13);
    }
    else if constexpr (n == 15) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = value;
        detail::visit_fields(visitor, std::make_index_sequence<15>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
    }
    else if constexpr (n == 16) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15] = value;
        detail::visit_fields(visitor, std::make_index_sequence<16>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15);
    }
    else if constexpr (n == 17) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16] = value;
        detail::visit_fields(visitor, std::make_index_sequence<17>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16);
    }
    else if constexpr (n == 18) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17] = value;
        detail::visit_fields(visitor, std::make_index_sequence<18>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17);
    }
    else if constexpr (n == 19) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18] = value;
        detail::visit_fields(visitor, std::make_index_sequence<19>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18);
    }
    else if constexpr (n == 20) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19] = value;
        detail::visit_fields(visitor, std::make_index_sequence<20>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19);
    }
    else if constexpr (n == 21) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20] = value;
        detail::visit_fields(visitor, std::make_index_sequence<21>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20);
    }
    else if constexpr (n == 22) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21] = value;
        detail::visit_fields(visitor, std::make_index_sequence<22>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21);
    }
    else if constexpr (n == 23) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22] = value;
        detail::visit_fields(visitor, std::make_index_sequence<23>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22);
    }
    else if constexpr (n == 24) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23] = value;
        detail::visit_fields(visitor, std::make_index_sequence<24>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23);
    }
    else if constexpr (n == 25) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24] = value;
        detail::visit_fields(visitor, std::make_index_sequence<25>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24);
    }
    else if constexpr (n == 26) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25] = value;
        detail::visit_fields(visitor, std::make_index_sequence<26>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25);
    }
    else if constexpr (n == 27) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26] = value;
        detail::visit_fields(visitor, std::make_index_sequence<27>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26);
    }
    else if constexpr (n == 28) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27] = value;
        detail::visit_fields(visitor, std::make_index_sequence<28>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27);
    }
    else if constexpr (n == 29) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28] = value;
        detail::visit_fields(visitor, std::make_index_sequence<29>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28);
    }
    else if constexpr (n == 30) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29] = value;
        detail::visit_fields(visitor, std::make_index_sequence<30>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29);
    }
    else if constexpr (n == 31) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30] = value;
        detail::visit_fields(visitor, std::make_index_sequence<31>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30);
    }
    else if constexpr (n == 32) {
        auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31] = value;
        detail::visit_fields(visitor, std::make_index_sequence<32>{}, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31);
    }
}
} // namespace quidra::ir::fields
