#pragma once

// The numeric kinds of the language, described once.
//
// Owns: one row per numeric TypeKind with its spelling, family, width,
// signedness, IEEE format, tensor dtype, sort element kind, C ABI scalar and
// the operations it supports. The numeric predicates of quidra/types.hpp, the
// dtype mapping of quidra/ir/dtype.hpp and the frontend's numeric name sets
// read the rows, so a numeric kind is described by its row alone.

#include "quidra/abi/dtype.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/type_kind.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace quidra {

// The family a numeric kind belongs to. Values of different families never
// convert implicitly.
enum class NumericFamily {
    Natural,
    Integer,
    Real,
    Complex
};

// The IEEE 754 binary interchange format of a fixed-width real kind.
enum class IeeeFormat {
    None,
    Binary16,
    Bfloat16,
    Binary32,
    Binary64
};

// The C scalar type a fixed-width kind has at a C boundary.
enum class CAbiScalar {
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Double
};

struct NumericKindInfo {
    TypeKind kind;
    // The type's name as type_name renders it.
    std::string_view spelling;
    NumericFamily family;
    // Bits of a fixed-width value; 0 for an arbitrary-precision kind.
    unsigned width;
    bool is_signed;
    IeeeFormat format;
    // The tensor element dtype; fixed-width kinds only.
    std::optional<abi::Dtype> dtype;
    // The element kind of a sorted array of the kind, if the runtime sorts it.
    std::optional<abi::SortElementKind> sort_kind;
    std::optional<CAbiScalar> c_abi;
    // AND, OR, XOR, NOT and the shifts apply to the kind.
    bool bitwise;
    // The kind can be a tensor element.
    bool tensor_element;
    // Tensors of the kind can be tracked by autograd.
    bool differentiable;
};

inline constexpr std::array<NumericKindInfo, 13> numeric_kinds{{
    {TypeKind::Int64, "int64", NumericFamily::Integer, 64, true, IeeeFormat::None,
     abi::Dtype::int64, abi::SortElementKind::int64, CAbiScalar::Int64, true, true, false},
    {TypeKind::Int8, "int8", NumericFamily::Integer, 8, true, IeeeFormat::None,
     abi::Dtype::int8, abi::SortElementKind::int8, CAbiScalar::Int8, true, true, false},
    {TypeKind::Int16, "int16", NumericFamily::Integer, 16, true, IeeeFormat::None,
     abi::Dtype::int16, abi::SortElementKind::int16, CAbiScalar::Int16, true, true, false},
    {TypeKind::Int32, "int32", NumericFamily::Integer, 32, true, IeeeFormat::None,
     abi::Dtype::int32, abi::SortElementKind::int32, CAbiScalar::Int32, true, true, false},
    {TypeKind::Nat8, "nat8", NumericFamily::Natural, 8, false, IeeeFormat::None,
     abi::Dtype::uint8, abi::SortElementKind::uint8, CAbiScalar::UInt8, true, true, false},
    {TypeKind::Nat16, "nat16", NumericFamily::Natural, 16, false, IeeeFormat::None,
     abi::Dtype::uint16, abi::SortElementKind::uint16, CAbiScalar::UInt16, true, true, false},
    {TypeKind::Nat32, "nat32", NumericFamily::Natural, 32, false, IeeeFormat::None,
     abi::Dtype::uint32, abi::SortElementKind::uint32, CAbiScalar::UInt32, true, true, false},
    {TypeKind::Nat64, "nat64", NumericFamily::Natural, 64, false, IeeeFormat::None,
     abi::Dtype::uint64, abi::SortElementKind::uint64, CAbiScalar::UInt64, true, true, false},
    {TypeKind::Int, "int", NumericFamily::Integer, 0, true, IeeeFormat::None,
     std::nullopt, abi::SortElementKind::bare_integer, std::nullopt, false, false, false},
    {TypeKind::Nat, "nat", NumericFamily::Natural, 0, false, IeeeFormat::None,
     std::nullopt, abi::SortElementKind::bare_integer, std::nullopt, false, false, false},
    {TypeKind::Real64, "real64", NumericFamily::Real, 64, true, IeeeFormat::Binary64,
     abi::Dtype::float64, abi::SortElementKind::float64, CAbiScalar::Double, false, true, true},
    {TypeKind::Real32, "real32", NumericFamily::Real, 32, true, IeeeFormat::Binary32,
     abi::Dtype::float32, abi::SortElementKind::float32, CAbiScalar::Float, false, true, true},
    {TypeKind::Real, "real", NumericFamily::Real, 0, true, IeeeFormat::None,
     std::nullopt, std::nullopt, std::nullopt, false, false, false},
}};

namespace numeric_types_detail {

// TypeKind::Nat is the last enumerator (type_kind.hpp appends new kinds).
inline constexpr std::size_t type_kind_count = static_cast<std::size_t>(TypeKind::Nat) + 1;

// The row of each TypeKind in numeric_kinds, or -1.
inline constexpr auto rows = [] {
    std::array<int, type_kind_count> result{};
    for (auto& row : result) row = -1;
    for (std::size_t i = 0; i < numeric_kinds.size(); ++i) {
        result[static_cast<std::size_t>(numeric_kinds[i].kind)] = static_cast<int>(i);
    }
    return result;
}();

} // namespace numeric_types_detail

// The row of `kind`, or nullptr for a kind that is not numeric.
constexpr const NumericKindInfo* numeric_kind_info(TypeKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= numeric_types_detail::type_kind_count) return nullptr;
    const auto row = numeric_types_detail::rows[index];
    return row < 0 ? nullptr : &numeric_kinds[static_cast<std::size_t>(row)];
}

// The row whose spelling is `spelling` (a canonical type name), or nullptr.
constexpr const NumericKindInfo* numeric_kind_info(std::string_view spelling) {
    for (const auto& info : numeric_kinds) {
        if (info.spelling == spelling) return &info;
    }
    return nullptr;
}

// The significand precision in bits of an IEEE format (0 for None).
constexpr int ieee_precision_bits(IeeeFormat format) {
    switch (format) {
        case IeeeFormat::Binary16: return 11;
        case IeeeFormat::Bfloat16: return 8;
        case IeeeFormat::Binary32: return 24;
        case IeeeFormat::Binary64: return 53;
        case IeeeFormat::None: break;
    }
    return 0;
}

static_assert(
    [] {
        for (std::size_t i = 0; i < numeric_kinds.size(); ++i) {
            const auto& info = numeric_kinds[i];
            if (numeric_kind_info(info.kind) != &info) return false;
            const bool fixed = info.width != 0;
            const bool real = info.family == NumericFamily::Real;
            if ((info.format != IeeeFormat::None) != (fixed && real)) return false;
            if (info.dtype.has_value() != info.tensor_element) return false;
            if (info.tensor_element && !fixed) return false;
            if (info.bitwise && (!fixed || real)) return false;
            if (info.differentiable && !(fixed && real)) return false;
            if (info.family == NumericFamily::Natural && info.is_signed) return false;
            if (info.dtype && abi::dtype_info(*info.dtype).bytes * 8 != static_cast<int>(info.width)) {
                return false;
            }
        }
        return true;
    }(),
    "every numeric kind has one consistent row");

// Runtime messages spell a conversion's destination type from the abi
// tables (abi::DtypeInfo::quidra_name, abi::conversion_type_name); they must
// spell every numeric kind as its row does.
static_assert(
    [] {
        for (const auto& info : numeric_kinds) {
            if (info.dtype && abi::dtype_info(*info.dtype).quidra_name != info.spelling) return false;
        }
        return numeric_kind_info(TypeKind::Int)->spelling ==
                   abi::conversion_type_name(abi::conversion_type_integer) &&
               numeric_kind_info(TypeKind::Nat)->spelling ==
                   abi::conversion_type_name(abi::conversion_type_natural) &&
               numeric_kind_info(TypeKind::Real)->spelling ==
                   abi::conversion_type_name(abi::conversion_type_real);
    }(),
    "the runtime spells every numeric type as its row does");

} // namespace quidra
