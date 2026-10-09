#pragma once

// HttpLowering: http.get and a response's header(). Defined in
// http_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class HttpLowering {
public:
    HttpLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, const EnclosingClass& enclosing_class,
        LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          enclosing_class_(enclosing_class), lifetime_(lifetime) {}

    // http.get and a response's header()
    ValueId lower_http_get(const Expr& e, const CallExpr& n);
    ValueId lower_http_header(const Expr& e, const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    const EnclosingClass& enclosing_class_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
