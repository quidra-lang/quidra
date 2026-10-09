#pragma once

// AssignmentLowering: compound, in-place append and plain assignments.
// Defined in assignment_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/assignment_order.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/shape_constraint_state.hpp"
#include "lowering/storage_writes.hpp"
#include <cstddef>
#include <optional>
#include <vector>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ShapeConstraintLowering;

class AssignmentLowering {
public:
    AssignmentLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, const LocalScope& scope,
        CheckElisionFacts& facts, const EnclosingClass& enclosing_class,
        const ShapeConstraintState& shapes,
        const StorageWrites& storage_writes,
        const AssignmentOrder& assignment_order, LifetimeLowering& lifetime,
        ShapeConstraintLowering& shape_constraints)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), facts_(facts), enclosing_class_(enclosing_class),
          shapes_(shapes), storage_writes_(storage_writes),
          assignment_order_(assignment_order),
          lifetime_(lifetime), shape_constraints_(shape_constraints) {}

    // an assignment statement
    void lower_assignment(const Stmt& s, const AssignStmt& n);

private:
    // the kinds of assignment, which lower_assignment dispatches to
    void lower_compound_assignment(
        const Stmt& s, const AssignStmt& n, const Type& t);
    // A compound assignment whose right-hand side may write the target's root
    // storage: the target is resolved again after the right-hand side.
    void lower_compound_store_into_current_storage(
        const Stmt& s, const AssignStmt& n, const Type& t);
    // Whether the target's path can be resolved a second time: fields and
    // array elements from a root binding.
    bool target_path_replayable(const Expr& expression) const;
    // The address of a field or element target, and the value of a base on
    // its path, lowered as lower_address and lower do. The first resolution
    // records the index values in `indices`; a replay (`replay` set) reads
    // them back in the same order instead of evaluating the indices again.
    ValueId lower_target_address(
        const Expr& target, std::vector<ValueId>& indices,
        std::optional<std::size_t>& replay);
    ValueId lower_path_value(
        const Expr& expression, std::vector<ValueId>& indices,
        std::optional<std::size_t>& replay);
    bool lower_string_append_assignment(const AssignStmt& n, const Type& t);
    bool lower_array_append_assignment(
        const Stmt& s, const AssignStmt& n, const Type& t);
    void lower_plain_assignment(
        const Stmt& s, const AssignStmt& n, const Type& t);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    const LocalScope& scope_;
    CheckElisionFacts& facts_;
    const EnclosingClass& enclosing_class_;
    const ShapeConstraintState& shapes_;
    const StorageWrites& storage_writes_;
    const AssignmentOrder& assignment_order_;
    LifetimeLowering& lifetime_;
    ShapeConstraintLowering& shape_constraints_;
};

} // namespace quidra::lowering
