#pragma once

// ConsoleLowering: print and flush. Defined in console_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class ConsoleLowering {
public:
    ConsoleLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime) {}

    // print, flush
    ValueId lower_print(const Expr& e, const CallExpr& n);
    ValueId lower_flush(const Expr& e);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
