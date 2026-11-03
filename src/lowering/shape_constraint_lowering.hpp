#pragma once

// ShapeConstraintLowering: the extents a declaration, parameter or return
// type spells: evaluated once into hidden locals (ShapeConstraintState),
// then checked, or used to shape a tensor allocation. Defined in
// shape_constraint_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/shape_constraint_state.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace quidra::lowering {

class Lowerer;

class ShapeConstraintLowering {
public:
    ShapeConstraintLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope, bool force_checks = false)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), proof_(checked, force_checks) {}

    // declared extents and their checks
    CapturedExtents capture_extents(
        const std::vector<std::shared_ptr<Expr>>& expressions,
        std::string_view prefix);
    void emit_shaped_constraint(
        ValueId value, TypeKind kind,
        const CapturedExtents& captured,
        SourceSpan span, const Expr* expression = nullptr);
    void emit_array_constraints(
        ValueId array, const Type& array_type,
        const std::vector<std::optional<std::string>>& captured,
        std::size_t axis, SourceSpan span, const Expr* expression = nullptr);
    ValueId generated_shape_array(
        const std::vector<std::optional<std::string>>& captured,
        SourceSpan span);

    // A brace shape (L17), as the checker planned it: per axis the hidden
    // Int locals that hold the extent the axis must have, per shape variable
    // the one that holds its value.
    struct BraceShape {
        CapturedExtents extents;
        std::vector<std::optional<std::string>> variables;
    };
    // Before the initializer: the expressions over no variable of the shape,
    // left to right (today's capture order), and the `nat N = EXPR`
    // variables among them.
    BraceShape capture_brace_shape(const ShapePlan& plan, SourceSpan span, bool bind = true,
                                   const BraceShape* reserved = nullptr);
    void adopt_shape_variable(const std::string& name) { prebound_variables_.insert(name); }
    // After it: the extents of the `nat N` variables something reads, the
    // expressions over the shape's own variables in dependency order, then
    // the check of `value` (when there is one) against every constrained
    // axis. `subject` is the binding a failure names.
    void finish_brace_shape(const ShapePlan& plan, BraceShape& shape,
                            std::optional<ValueId> value, TypeKind kind,
                            const std::string& subject, SourceSpan span, const Expr* expression = nullptr);
    std::vector<CapturedExtents> capture_signature_shape(
        const SignatureShapeUnit& unit, const ShapePlan& plan,
        const std::vector<ValueId>& parameters, SourceSpan span,
        const std::vector<ProvenShapeConstraints>* proofs = nullptr);
    // A match case's brace shape: the extents of its `nat N` variables read
    // from the payload `value` and, when the case is conditional, its tests,
    // which branch to `next` when one fails; then its variables are bound.
    void lower_case_shape(const ShapePlan& plan, ValueId value, TypeKind kind,
                          const std::string& next, SourceSpan span);

    void initialize_captured_extents(const std::vector<std::shared_ptr<Expr>>& expressions,
                                    const std::vector<std::optional<std::string>>& captured);
private:
    // One extent into a hidden Int local named after `prefix`; nat(e) over
    // an integer e is captured as e.
    std::string capture_extent(const Expr& expression, std::string_view prefix,
                               const std::optional<std::string>& reserved = std::nullopt);
    // Binds the shape variable `name` as a nat local holding the Int local
    // `extent` (checked when it may be negative).
    void bind_shape_variable(const std::string& name, const std::string& extent,
                             bool checked, SourceSpan span);
    // one extent's value, the captured extents loaded back, and whether an
    // array constraint goes deeper than `axis`
    std::unordered_set<std::string> prebound_variables_;
    ValueId extent_value(const Expr& expression);
    ValueId shape_axis(ValueId dimensions, std::size_t axis, std::size_t explicit_rank,
                       std::optional<std::size_t> ellipsis, SourceSpan span);
    std::vector<std::optional<ValueId>> load_captured_extents(
        const std::vector<std::optional<std::string>>& captured);
    bool deeper_array_constraint(
        const std::vector<std::optional<std::string>>& captured,
        std::size_t axis) const;

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    ShapeProof proof_;
};

} // namespace quidra::lowering
