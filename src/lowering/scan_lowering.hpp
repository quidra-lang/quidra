#pragma once

// ScanLowering: the stdlib scan() intrinsic. Defined in scan_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class ScanLowering {
public:
    ScanLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), lifetime_(lifetime) {}

    // scan()
    ValueId lower_scan(const Expr& e);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
