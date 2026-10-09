#pragma once

// ConcurrencyLowering: atomic.Counter, atomic.counter() and task.all.
// Defined in concurrency_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class ConcurrencyLowering {
public:
    ConcurrencyLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime) {}

    // atomic.Counter, atomic.counter(), task.all
    std::optional<ValueId> try_counter_method(
        const Expr& e, const MethodCallExpr& n, const Type& receiver_type);
    ValueId lower_atomic_counter(const CallExpr& n);
    ValueId lower_task_all(const Expr& e, const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
