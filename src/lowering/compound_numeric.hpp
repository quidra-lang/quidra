#pragma once
#include "lowering/conversion_lowering.hpp"
#include "lowering/finite_checks.hpp"
#include "lowering/operator_lowering.hpp"
#include "lowering/ownership_expansion.hpp"

namespace quidra::lowering {

inline std::optional<Type> numeric_compound_type(const Type& left, const Type& right) {
    const auto* lhs = &left;
    const auto* rhs = &right;
    while (lhs->kind == TypeKind::Array) lhs = lhs->first.get();
    while (rhs->kind == TypeKind::Array) rhs = rhs->first.get();
    const auto common = numeric_common_type(*lhs, *rhs);
    if (!common) return std::nullopt;
    const auto shape = [&](const auto& self, const Type& type) -> Type {
        return type.kind == TypeKind::Array
            ? Type::array(self(self, *type.first), type.length) : *common;
    };
    return shape(shape, left);
}

// Operands follow ordinary arithmetic common-type rules. Only the result
// converts to the storage type, matching `place = place op rhs`.
inline ir::ValueId checked_numeric_compound(
    ir::FunctionBuilder& builder, ConversionLowering& conversions,
    OperatorLowering& operators, const std::string& op,
    ir::ValueId old, const Type& destination, ir::ValueId rhs, const Type& source,
    const Type& calculation, SourceSpan span) {
    const SourcePosition site{static_cast<std::uint32_t>(span.start.line),
                              static_cast<std::uint32_t>(span.start.column)};
    ir::ValueId result;
    if (destination.kind == TypeKind::Array) {
        result = operators.lower_array_elementwise_values(
            op, old, destination, rhs, source, calculation, site);
    } else {
        const auto left = destination == calculation ? old
            : conversions.convert_numeric_argument(old, destination, calculation, span);
        const auto right = source == calculation ? rhs
            : conversions.convert_numeric_argument(rhs, source, calculation, span);
        result = builder.fresh();
        emit_checked_binary(builder, ir::Binary{result, op, left, right,
            calculation, calculation, site.line, site.column});
        if (left != old) release_owned(builder, left, calculation);
        if (right != rhs) release_owned(builder, right, calculation);
    }
    if (calculation == destination) return result;
    const auto converted = conversions.convert_numeric_argument(result, calculation, destination, span);
    release_owned(builder, result, calculation);
    return converted;
}

} // namespace quidra::lowering
