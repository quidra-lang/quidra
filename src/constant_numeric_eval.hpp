#pragma once

// Compile-time values of numeric literals.
//
// Owns: the value a numeric literal takes in a fixed-width real type, which
// is its decimal value rounded once, correctly, to the type's IEEE format
// (ieee_decimal.hpp), and the value of one arithmetic operation of that type
// on such values. The checker tests literals' ranges and constant real
// expressions with it and the backend materializes literal constants with
// it, so both see the same value. It reads only types and literal text, so
// every compiler stage can include it.

#include "ieee_decimal.hpp"
#include "quidra/types.hpp"

#include <limits>
#include <optional>
#include <string_view>

namespace quidra::constant_eval {

// The IEEE format of a fixed-width real type.
inline std::optional<ieee_decimal::Format> ieee_format(const Type& type) {
    const auto* numeric = numeric_kind_info(type.kind);
    if (!numeric) return std::nullopt;
    switch (numeric->format) {
        case IeeeFormat::Binary16: return ieee_decimal::binary16;
        case IeeeFormat::Bfloat16: return ieee_decimal::bfloat16;
        case IeeeFormat::Binary32: return ieee_decimal::binary32;
        case IeeeFormat::Binary64: return ieee_decimal::binary64;
        case IeeeFormat::None: break;
    }
    return std::nullopt;
}

// The value of the decimal literal text `spelling` (a real literal, or the
// digits of an integer literal) in the fixed-width real type `type`, widened
// exactly to binary64. nullopt when the value rounds beyond the type's
// finite range, when `type` is not a fixed-width real type or when
// `spelling` is not decimal text.
inline std::optional<double> real_literal_value(std::string_view spelling, const Type& type) {
    const auto format = ieee_format(type);
    if (!format) return std::nullopt;
    if (spelling == "inf" || spelling == "+inf")
        return std::numeric_limits<double>::infinity();
    if (spelling == "-inf") return -std::numeric_limits<double>::infinity();
    ieee_decimal::Decimal decimal;
    if (!ieee_decimal::parse(spelling, decimal)) return std::nullopt;
    const auto rounded = ieee_decimal::round(decimal, *format);
    if (rounded.overflow) return std::nullopt;
    return ieee_decimal::binary64_value(rounded, *format);
}

// The value of `left op right` (op one of + - * /) in the fixed-width real
// type `type`, whose values both operands are, widened exactly to binary64:
// the exact result rounded once to the type's format, as the generated code
// computes it; an infinity or NaN where the result is not finite. nullopt
// for another type or operator. binary64 arithmetic gives the binary64
// result directly, and rounds the exact result of binary32 operands
// correctly to binary32 when narrowed (53 >= 2 * 24 + 2 bits).
inline std::optional<double> real_operation_value(char op, double left, double right,
                                                  const Type& type) {
    const auto format = ieee_format(type);
    if (!format) return std::nullopt;
    double result = 0.0;
    switch (op) {
        case '+': result = left + right; break;
        case '-': result = left - right; break;
        case '*': result = left * right; break;
        case '/': result = left / right; break;
        default: return std::nullopt;
    }
    // The exact result of 16-bit operands is binary64's (or rounded within
    // its 53 bits, which rounds correctly to 8 or 11 again); rounded once to
    // the type, an infinity beyond its range.
    return float_value_in_type(result, type);
}

} // namespace quidra::constant_eval
