#pragma once

// ShapeConstraintLowering: the extents a declaration, parameter or return
// type spells: evaluated once into hidden locals (ShapeConstraintState),
// then checked, or used to shape a tensor allocation. Defined in
// shape_constraint_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::lowering {

class Lowerer;

class ShapeConstraintLowering {
public:
    ShapeConstraintLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope) {}

    // declared extents and their checks
    std::vector<std::optional<std::string>> capture_extents(
        const std::vector<std::shared_ptr<Expr>>& expressions,
        std::string_view prefix);
    void emit_shaped_constraint(
        ValueId value, TypeKind kind,
        const std::vector<std::optional<std::string>>& captured,
        SourceSpan span);
    void emit_array_constraints(
        ValueId array, const Type& array_type,
        const std::vector<std::optional<std::string>>& captured,
        std::size_t axis, SourceSpan span);
    ValueId generated_shape_array(
        const std::vector<std::optional<std::string>>& captured,
        SourceSpan span);

private:
    // one extent's value, the captured extents loaded back, and whether an
    // array constraint goes deeper than `axis`
    ValueId extent_value(const Expr& expression);
    std::vector<std::optional<ValueId>> load_captured_extents(
        const std::vector<std::optional<std::string>>& captured);
    bool deeper_array_constraint(
        const std::vector<std::optional<std::string>>& captured,
        std::size_t axis) const;

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
};

} // namespace quidra::lowering
