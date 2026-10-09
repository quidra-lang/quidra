#pragma once

// AutogradLowering: a tensor's .grad and autograd methods, autograd.Target,
// and the targets reached from a value. Defined in autograd_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <optional>
#include <string>
#include <unordered_set>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ReflectionLowering;

class AutogradLowering {
public:
    AutogradLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime,
        ReflectionLowering& reflection)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime), reflection_(reflection) {}

    // a tensor's .grad and autograd methods, autograd.Target and
    // autograd.target()
    ValueId lower_tensor_grad(const Expr& e, const MemberExpr& n);
    std::optional<ValueId> try_autograd_target_method(
        const Expr& e, const MethodCallExpr& n, const Type& receiver_type);
    std::optional<ValueId> try_tensor_autograd_method(
        const Expr& e, const MethodCallExpr& n, ValueId receiver,
        const Type& receiver_type, bool receiver_owned);
    ValueId lower_autograd_target();

private:
    // the targets reached from a value
    bool autograd_target_type_contains(
        const Type& type,std::unordered_set<std::string>& active) const;
    void collect_autograd_targets(
        ValueId object,const Type& type,const std::string& destination,
        std::unordered_set<std::string>& active);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
    ReflectionLowering& reflection_;
};

} // namespace quidra::lowering
