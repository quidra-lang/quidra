#pragma once

// CheckLowering: test.check and test.equal. Defined in check_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class CheckLowering {
public:
    CheckLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime) {}

    // test.check, test.equal
    ValueId lower_test_check(const CallExpr& n);
    ValueId lower_test_equal(const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
