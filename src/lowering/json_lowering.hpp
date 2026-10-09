#pragma once

// JsonLowering: json.parse and a JSON value's methods. Defined in
// json_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class JsonLowering {
public:
    JsonLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, const EnclosingClass& enclosing_class,
        LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          enclosing_class_(enclosing_class), lifetime_(lifetime) {}

    // json.parse and a JSON value's methods
    ValueId lower_json_parse(const Expr& e, const CallExpr& n);
    ValueId lower_json_kind();
    ValueId lower_json_size(const Expr& e);
    ValueId lower_json_get(const Expr& e, const CallExpr& n);
    ValueId lower_json_at(const Expr& e, const CallExpr& n);
    ValueId lower_json_text(const Expr& e);
    ValueId lower_json_integer(const Expr& e);
    ValueId lower_json_number(const Expr& e);
    ValueId lower_json_big_real(const Expr& e);
    ValueId lower_json_boolean(const Expr& e);
    ValueId lower_json_encode();
    ValueId lower_json_equal(const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    const EnclosingClass& enclosing_class_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
