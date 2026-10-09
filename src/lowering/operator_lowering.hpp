#pragma once

// OperatorLowering: binary operators and the exact-number builtins. Defined
// in operator_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ConversionLowering;

class OperatorLowering {
public:
    OperatorLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        const CheckElisionFacts& facts, LifetimeLowering& lifetime,
        ConversionLowering& conversions)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), facts_(facts), lifetime_(lifetime),
          conversions_(conversions) {}

    // binary operators
    ValueId lower_binary(const Expr& e, const BinaryExpr& n);

    // the exact-number builtins
    ValueId lower_exact_atom(const Expr& e, const CallExpr& n);
    ValueId lower_exact_unary(const Expr& e, const CallExpr& n);

private:
    // the mechanisms of a binary operator, which lower_binary tries in
    // this order
    std::optional<ValueId> lower_empty_string_test(const BinaryExpr& n);
    std::optional<ValueId> lower_ascii_character_test(const BinaryExpr& n);
    ValueId lower_string_concatenation(const Expr& e);
    ValueId lower_short_circuit(const BinaryExpr& n);
    ValueId lower_operator_tree(const Expr& e);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    const CheckElisionFacts& facts_;
    LifetimeLowering& lifetime_;
    ConversionLowering& conversions_;
};

} // namespace quidra::lowering
