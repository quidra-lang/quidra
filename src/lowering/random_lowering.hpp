#pragma once

// RandomLowering: random.generator and a generator's methods. Defined in
// random_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;

class RandomLowering {
public:
    RandomLowering(
        Lowerer& lowerer, ir::FunctionBuilder& builder,
        const EnclosingClass& enclosing_class)
        : lowerer_(lowerer), builder_(builder),
          enclosing_class_(enclosing_class) {}

    // random.generator and a generator's methods
    ValueId lower_random_generator(const CallExpr& n);
    ValueId lower_random_int(const Expr& e, const CallExpr& n);
    ValueId lower_random_float();
    ValueId lower_random_bool();

private:
    Lowerer& lowerer_;
    ir::FunctionBuilder& builder_;
    const EnclosingClass& enclosing_class_;
};

} // namespace quidra::lowering
