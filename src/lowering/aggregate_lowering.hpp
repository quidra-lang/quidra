#pragma once

// AggregateLowering: the methods of arrays, array(count, fill) and len().
// Defined in aggregate_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ConversionLowering;

class AggregateLowering {
public:
    AggregateLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        LifetimeLowering& lifetime, ConversionLowering& conversions)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), lifetime_(lifetime), conversions_(conversions) {}

    // arrays
    std::optional<ValueId> try_array_method(
        const Expr& e, const MethodCallExpr& n, const Type& receiver_type);
    ValueId lower_array_allocation(const Expr& e, const CallExpr& n);
    ValueId lower_len(const Expr& e, const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    LifetimeLowering& lifetime_;
    ConversionLowering& conversions_;
};

} // namespace quidra::lowering
