#pragma once

// TextLowering: the methods of strings and of the string type, and the
// implicit .string(). Defined in text_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class TextLowering {
public:
    TextLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime) {}

    // strings, the string type, the implicit .string()
    std::optional<ValueId> try_string_type_method(const Expr& e, const MethodCallExpr& n);
    std::optional<ValueId> try_string_method(
        const Expr& e, const MethodCallExpr& n, const Type& receiver_type);
    std::optional<ValueId> try_implicit_string_method(const Expr& e, const MethodCallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
