#pragma once

// CheckLowering: test.check and test.equal. Defined in check_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;

class CheckLowering {
public:
    CheckLowering(Lowerer& lowerer, ir::FunctionBuilder& builder)
        : lowerer_(lowerer), builder_(builder) {}

    // test.check, test.equal
    ValueId lower_test_check(const CallExpr& n);
    ValueId lower_test_equal(const CallExpr& n);

private:
    Lowerer& lowerer_;
    ir::FunctionBuilder& builder_;
};

} // namespace quidra::lowering
