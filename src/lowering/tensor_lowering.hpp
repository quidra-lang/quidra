#pragma once

// TensorLowering: the methods of tensors, tensor creation and gpu.sync.
// Defined in tensor_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/shape_constraint_state.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ShapeConstraintLowering;
class AutogradLowering;

class TensorLowering {
public:
    TensorLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, const ShapeConstraintState& shapes,
        LifetimeLowering& lifetime, ShapeConstraintLowering& shape_constraints,
        AutogradLowering& autograd)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          shapes_(shapes), lifetime_(lifetime),
          shape_constraints_(shape_constraints), autograd_(autograd) {}

    // tensors
    std::optional<ValueId> try_tensor_method(
        const Expr& e, const MethodCallExpr& n, const Type& receiver_type);
    ValueId lower_tensor_create(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);
    ValueId lower_gpu_sync(const Expr& e, const CallExpr& n);

private:
    // An integer array argument as the int64[] the runtime reads: when its
    // elements are of another kind it is a converted copy, which `release`
    // releases after the call.
    ValueId lower_runtime_array(const Expr& argument, bool& converted);
    void release_runtime_array(const Expr& argument, ValueId value, bool converted);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    const ShapeConstraintState& shapes_;
    LifetimeLowering& lifetime_;
    ShapeConstraintLowering& shape_constraints_;
    AutogradLowering& autograd_;
};

} // namespace quidra::lowering
