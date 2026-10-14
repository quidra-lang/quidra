#pragma once

// The finite checks of fixed-width real arithmetic: no value of a real16,
// real16b, real32 or real64 type that the program can observe is NaN or an
// infinity. Of the scalar operations, +, -, *, / and ^ can turn finite
// operands into such a value (an overflow, a zero divisor); every one of
// them is followed by the FiniteCheck of its result. Unary minus, the
// comparisons and the conversions (which check their own ranges) cannot.
//
// The optimizer merges the checks of one expression and sinks the check of
// a loop (optimizer/finite_check_merging.hpp); the lowering emits one check
// per operation, which is complete on its own.

#include "ir/function_builder.hpp"
#include "quidra/ir/check.hpp"
#include "quidra/ir/numeric.hpp"
#include "quidra/types.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace quidra::lowering {

// The cause of a real operation that can produce a non-finite value from
// finite operands, or nullopt.
inline std::optional<ir::FiniteCause> finite_cause(std::string_view op, const Type& operand) {
    if (!is_fixed_real(operand)) return std::nullopt;
    if (op == "+") return ir::FiniteCause::addition;
    if (op == "-") return ir::FiniteCause::subtraction;
    if (op == "*") return ir::FiniteCause::multiplication;
    if (op == "/") return ir::FiniteCause::division;
    if (op == "^") return ir::FiniteCause::power;
    return std::nullopt;
}

// Emits `binary`, then the FiniteCheck of its result when the operation can
// produce a non-finite value.
inline void emit_checked_binary(ir::FunctionBuilder& builder, ir::Binary binary) {
    const auto cause = finite_cause(binary.op, binary.operand_type);
    ir::CheckedReal checked;
    if (cause) {
        checked = ir::CheckedReal{binary.out,  binary.result_type, binary.position,
                                  *cause,      binary.left,        binary.right};
    }
    builder.emit(std::move(binary));
    if (cause) builder.emit(ir::FiniteCheck{{std::move(checked)}, {}});
}

} // namespace quidra::lowering
