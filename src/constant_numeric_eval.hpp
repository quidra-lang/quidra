#pragma once

// Compile-time values of numeric literals.
//
// Owns: the value a numeric literal takes in a fixed-width real type, which
// is its decimal value rounded once, correctly, to the type's IEEE format
// (ieee_decimal.hpp). The checker tests literals' ranges with it and the
// backend materializes literal constants with it, so both see the same value.
// It reads only types and literal text, so every compiler stage can include
// it.

#include "ieee_decimal.hpp"
#include "quidra/types.hpp"

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
    ieee_decimal::Decimal decimal;
    if (!format || !ieee_decimal::parse(spelling, decimal)) return std::nullopt;
    const auto rounded = ieee_decimal::round(decimal, *format);
    if (rounded.overflow) return std::nullopt;
    return ieee_decimal::binary64_value(rounded, *format);
}

} // namespace quidra::constant_eval
