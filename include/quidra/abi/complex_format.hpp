#pragma once

#include "quidra/abi/half_format.hpp"
#include <cmath>
#include <limits>
#include <type_traits>

namespace quidra::abi::complex {
using half::Real16;
using half::Real16b;
template <typename T>
inline constexpr bool half_element=std::is_same_v<T,Real16>||std::is_same_v<T,Real16b>;
// The element types of complex tensors: a real and an imaginary part of
// the real element type P. Each operation of P is rounded once to P (a
// binary32 or binary64 product passes through a volatile, so a compiler
// that contracts across statements cannot fuse it into a sum); * is
// (ac - bd) + (ad + bc)i and / Smith's algorithm, as generated code computes
// complex scalars (llvm_backend/complex_arithmetic.hpp). A quotient with a NaN
// intermediate is rejected even when a later operation would hide it.
// Infinite intermediates follow the extended-real arithmetic policy.
template <typename P>
struct ComplexElement {
    P re{};
    P im{};

    ComplexElement() = default;
    ComplexElement(P real, P imaginary) : re(real), im(imaginary) {}
    // T{0}, as the element code spells a zero of every type.
    explicit ComplexElement(int value) : re(P(static_cast<double>(value))), im() {}
};

template <typename T>
inline constexpr bool complex_element = false;
template <typename P>
inline constexpr bool complex_element<ComplexElement<P>> = true;

template <typename P>
P part_product(P a, P b) {
    if constexpr (std::is_floating_point_v<P>) {
        volatile P product = a * b;
        return product;
    } else {
        return a * b;
    }
}

template <typename P>
bool part_is_finite(P value) {
    if constexpr (half_element<P>) return quidra::abi::half::isfinite(value);
    else return std::isfinite(value);
}

template <typename P>
P part_nan() {
    if constexpr (half_element<P>) return P(std::numeric_limits<double>::quiet_NaN());
    else return std::numeric_limits<P>::quiet_NaN();
}

template <typename P>
ComplexElement<P> operator+(ComplexElement<P> x, ComplexElement<P> y) {
    const P re = x.re + y.re;
    const P im = x.im + y.im;
    return {re, im};
}

template <typename P>
ComplexElement<P> operator-(ComplexElement<P> x, ComplexElement<P> y) {
    const P re = x.re - y.re;
    const P im = x.im - y.im;
    return {re, im};
}

template <typename P>
ComplexElement<P> operator-(ComplexElement<P> x) {
    return {-x.re, -x.im};
}

template <typename P>
ComplexElement<P> operator*(ComplexElement<P> x, ComplexElement<P> y) {
    const P ac = part_product(x.re, y.re);
    const P bd = part_product(x.im, y.im);
    const P ad = part_product(x.re, y.im);
    const P bc = part_product(x.im, y.re);
    const P re = ac - bd;
    const P im = ad + bc;
    return {re, im};
}

template <typename P>
ComplexElement<P> operator/(ComplexElement<P> x, ComplexElement<P> y) {
    const P a = x.re, b = x.im, c = y.re, d = y.im;
    const auto magnitude = [](P v) { return v < P{} ? P(-v) : v; };
    bool finite = true;
    const auto tested = [&](P v) {
        finite = finite && !std::isnan(static_cast<double>(v));
        return v;
    };
    P t{}, re_n{}, im_n{};
    if (!(magnitude(c) < magnitude(d))) {
        const P r = tested(d / c);
        t = tested(c + tested(part_product(d, r)));
        re_n = tested(a + tested(part_product(b, r)));
        im_n = tested(b - tested(part_product(a, r)));
    } else {
        const P r = tested(c / d);
        t = tested(tested(part_product(c, r)) + d);
        re_n = tested(tested(part_product(a, r)) + b);
        im_n = tested(tested(part_product(b, r)) - a);
    }
    const P re = tested(re_n / t);
    const P im = tested(im_n / t);
    if (!finite) return {part_nan<P>(), part_nan<P>()};
    return {re, im};
}

template <typename P>
bool operator==(ComplexElement<P> x, ComplexElement<P> y) {
    return x.re == y.re && x.im == y.im;
}

template <typename P>
bool operator!=(ComplexElement<P> x, ComplexElement<P> y) {
    return !(x == y);
}


} // namespace quidra::abi::complex
