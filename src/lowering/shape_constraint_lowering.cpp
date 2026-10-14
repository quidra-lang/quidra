// Shape constraints: the extents a declaration, parameter or return type
// spells (Tensor[n, m], T[n][m]) are evaluated once into hidden Int
// locals, then checked against the value at binding (ShapedConstraintCheck,
// and ExtentEqualCheck over each nested array level), or used to shape a
// tensor allocation from its context.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <stdexcept>
#include <algorithm>
#include <functional>

namespace quidra::lowering {

ValueId ShapeConstraintLowering::shape_axis(
    ValueId dimensions, std::size_t axis, std::size_t explicit_rank,
    std::optional<std::size_t> ellipsis, SourceSpan span) {
    if (!ellipsis || axis < *ellipsis)
        return builder_.const_int(static_cast<long long>(axis));
    const auto rank = builder_.fresh();
    builder_.emit(ArrayLength{rank, dimensions});
    const auto result = builder_.fresh();
    builder_.emit(Binary{result, "-", rank,
        builder_.const_int(static_cast<long long>(explicit_rank - axis)),
        Type::simple(TypeKind::Int64), Type::simple(TypeKind::Int64),
        static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)});
    return result;
}

ValueId ShapeConstraintLowering::extent_value(const Expr& expression) {
    auto value = lowerer_.lower(expression);
    const auto source = type_of(checked_, expression);
    const auto target = Type::simple(TypeKind::Int64);
    if (source == target) return value;
    auto out = builder_.fresh();
    const bool checked_range =
        numeric_conversion_policy(source, target) ==
        NumericConversionPolicy::ExplicitRangeCheck;
    builder_.emit(NumericConvert{
        out, value, source, target, checked_range,
        {static_cast<std::uint32_t>(expression.span.start.line),
        static_cast<std::uint32_t>(expression.span.start.column)}});
    return out;
}

std::string ShapeConstraintLowering::capture_extent(
    const Expr& expression, std::string_view prefix, const std::optional<std::string>& reserved) {
    const Expr* operand = &expression;
    if (const auto found = checked_.extent_capture_operands.find(&expression);
        found != checked_.extent_capture_operands.end()) {
        operand = found->second;
    }
    const auto name = reserved ? *reserved : builder_.hidden(prefix);
    if(!reserved) {
        scope_.local_type(name) = Type::simple(TypeKind::Int64);
        builder_.emit(DeclareLocal{name, Type::simple(TypeKind::Int64), {}, 0, 0});
    }
    const auto value = extent_value(*operand);
    builder_.emit(
        StoreLocal{name, value, Type::simple(TypeKind::Int64)});
    return name;
}

CapturedExtents ShapeConstraintLowering::capture_extents(
    const std::vector<std::shared_ptr<Expr>>& expressions,
    std::string_view prefix) {
    CapturedExtents captured;
    captured.reserve(expressions.size());
    for (const auto& expression : expressions) {
        if (!expression) {
            captured.push_back(std::nullopt);
            continue;
        }
        captured.push_back(capture_extent(*expression, prefix));
    }
    return captured;
}

void ShapeConstraintLowering::bind_shape_variable(
    const std::string& name, const std::string& extent, bool checked, SourceSpan span) {
    const auto nat = Type::simple(TypeKind::Nat);
    const bool prebound=prebound_variables_.contains(name);
    const auto local = prebound ? scope_.source_local(name) : scope_.bind_shape_variable(builder_, name, nat);
    if(!prebound) builder_.emit(DeclareLocal{
        local, nat, name, static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)});
    auto word = builder_.fresh();
    builder_.emit(LoadLocal{word, extent, Type::simple(TypeKind::Int64)});
    auto value = builder_.fresh();
    builder_.emit(NumericConvert{
        value, word, Type::simple(TypeKind::Int64), nat, checked,
        {static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)}, !checked});
    builder_.emit(StoreLocal{local, value, nat});
}

ShapeConstraintLowering::BraceShape ShapeConstraintLowering::capture_brace_shape(
    const ShapePlan& plan, SourceSpan span, bool bind, const BraceShape* reserved) {
    BraceShape shape=reserved ? *reserved : BraceShape{};
    shape.extents.resize(plan.axes.size());
    shape.extents.ellipsis = plan.ellipsis;
    shape.variables.resize(plan.variables.size());
    for (std::size_t axis = 0; axis < plan.axes.size(); ++axis) {
        const auto& current = plan.axes[axis];
        if (current.check != ShapeAxisCheck::Value || current.after_initializer) continue;
        const auto local = capture_extent(*current.capture, "shape.extent", shape.extents[axis]);
        shape.extents[axis] = local;
        if (!current.variable.empty() && current.variable_index < plan.variables.size() &&
            plan.variables[current.variable_index].axis == axis &&
            plan.variables[current.variable_index].value) {
            // `nat N = EXPR`: N is the value, which a negative nat(e) cannot be.
            shape.variables[current.variable_index] = local;
            if (bind && plan.variables[current.variable_index].read)
                bind_shape_variable(current.variable, local, true, span);
        }
    }
    return shape;
}

void ShapeConstraintLowering::finish_brace_shape(
    const ShapePlan& plan, BraceShape& shape, std::optional<ValueId> value, TypeKind kind,
    const std::string& subject, SourceSpan span, const Expr* expression) {
    const auto line = static_cast<std::uint32_t>(span.start.line);
    const auto column = static_cast<std::uint32_t>(span.start.column);
    const auto int64 = Type::simple(TypeKind::Int64);
    const auto rank = plan.axes.size();

    // The extents of the `nat N` variables something reads, once the rank is
    // known to be the shape's.
    std::vector<std::size_t> reads;
    for (std::size_t i = 0; i < plan.variables.size(); ++i) {
        if (plan.variables[i].read && !plan.variables[i].value) reads.push_back(i);
    }
    if (!reads.empty() && value) {
        if (plan.rank_unknown && !proof_.rank(expression)) {
            builder_.emit(ShapedConstraintCheck{
                *value, kind, std::vector<std::optional<ValueId>>(rank), {line, column}, {}, {}, plan.ellipsis});
        }
        const auto shape_type = Type::array(int64, plan.ellipsis ? -1 : static_cast<long long>(rank));
        auto extents = builder_.fresh();
        builder_.emit(TensorShape{extents, *value, shape_type});
        for (const auto variable : reads) {
            const auto axis = plan.variables[variable].axis;
            auto extent = builder_.fresh();
            builder_.emit(ArrayGet{
                extent, extents, shape_axis(extents, axis, rank, plan.ellipsis, span), int64,
                {line, column}, true, true});
            const auto local = shape.extents[axis] ? *shape.extents[axis] : builder_.hidden("shape.extent");
            if(!shape.extents[axis]) {
                scope_.local_type(local) = int64;
                builder_.emit(DeclareLocal{local, int64, {}, 0, 0});
            }
            builder_.emit(StoreLocal{local, extent, int64});
            shape.variables[variable] = local;
            shape.extents[axis] = local;
            bind_shape_variable(plan.variables[variable].name, local, false, span);
        }
        release_owned(builder_,extents, shape_type);
    }

    // The expressions over the shape's own variables.
    for (const auto axis : plan.after_order) {
        const auto& current = plan.axes[axis];
        const auto local = capture_extent(*current.capture, "shape.extent", shape.extents[axis]);
        shape.extents[axis] = local;
        if (!current.variable.empty() && current.variable_index < plan.variables.size() &&
            plan.variables[current.variable_index].axis == axis &&
            plan.variables[current.variable_index].value) {
            shape.variables[current.variable_index] = local;
            if (plan.variables[current.variable_index].read)
                bind_shape_variable(current.variable, local, true, span);
        }
    }
    // Repeats.
    for (std::size_t axis = 0; axis < rank; ++axis) {
        const auto& current = plan.axes[axis];
        if (current.check == ShapeAxisCheck::Variable)
            shape.extents[axis] = shape.variables[current.variable_index];
    }
    if (!value) return;

    // One check of every constrained axis; an axis that a shape variable
    // fixes names it.
    std::vector<std::optional<ValueId>> loads;
    std::vector<std::string> variables;
    bool named = false;
    for (std::size_t axis = 0; axis < rank; ++axis) {
        const auto& current = plan.axes[axis];
        if (current.check == ShapeAxisCheck::None || !shape.extents[axis] || proof_.axis(expression, axis)) {
            loads.push_back(std::nullopt);
            variables.emplace_back();
            continue;
        }
        auto load = builder_.fresh();
        builder_.emit(LoadLocal{load, *shape.extents[axis], int64});
        loads.push_back(load);
        variables.push_back(current.variable);
        named = named || !current.variable.empty();
    }
    if (!named) variables.clear();
    if (proof_.rank(expression) && std::none_of(loads.begin(), loads.end(),
        [](const auto& extent) { return extent.has_value(); })) return;
    builder_.emit(ShapedConstraintCheck{
        *value, kind, std::move(loads), {line, column}, std::move(variables),
        named ? subject : std::string{}, plan.ellipsis});
}

std::vector<CapturedExtents>
ShapeConstraintLowering::capture_signature_shape(
    const SignatureShapeUnit& unit, const ShapePlan& plan,
    const std::vector<ValueId>& parameters, SourceSpan span,
    const std::vector<ProvenShapeConstraints>* proofs) {
    BraceShape shape;
    shape.extents.resize(plan.axes.size());
    shape.variables.resize(plan.variables.size());
    const auto int64 = Type::simple(TypeKind::Int64);
    const auto line = static_cast<std::uint32_t>(span.start.line);
    const auto column = static_cast<std::uint32_t>(span.start.column);
    // Verify rank before a plain variable reads parameter shape storage.
    for (const auto& part : unit.parts) {
        if (part.parameter == 0) continue;
        const bool reads = std::any_of(plan.variables.begin(), plan.variables.end(),
            [&](const auto& variable) {
                return variable.read && !variable.value && variable.axis >= part.offset &&
                    variable.axis < part.offset + part.size;
            });
        if (!reads) continue;
        if (proofs && part.parameter <= proofs->size() && (*proofs)[part.parameter - 1].rank) continue;
        builder_.emit(ShapedConstraintCheck{parameters.at(part.parameter - 1), TypeKind::Tensor,
            std::vector<std::optional<ValueId>>(part.size), {line, column}, {}, part.subject, part.ellipsis});
    }
    // Plain parameter declarations bind before any axis expression runs.
    for (const auto& part : unit.parts) {
        if (part.parameter == 0) continue;
        std::optional<ValueId> dimensions;
        const auto shape_type = Type::array(int64, part.ellipsis ? -1 : static_cast<long long>(part.size));
        for (std::size_t v = 0; v < plan.variables.size(); ++v) {
            const auto& variable = plan.variables[v];
            if (!variable.read || variable.value || variable.axis < part.offset ||
                variable.axis >= part.offset + part.size) continue;
            if (!dimensions) {
                dimensions = builder_.fresh();
                builder_.emit(TensorShape{*dimensions, parameters.at(part.parameter - 1), shape_type});
            }
            const auto extent = builder_.fresh();
            builder_.emit(ArrayGet{extent, *dimensions,
                shape_axis(*dimensions, variable.axis - part.offset, part.size, part.ellipsis, span),
                int64, {line, column}, true, true});
            const auto local = builder_.hidden("shape.extent");
            scope_.local_type(local) = int64;
            builder_.emit(DeclareLocal{local, int64, {}, 0, 0});
            builder_.emit(StoreLocal{local, extent, int64});
            shape.variables[v] = local;
            shape.extents[variable.axis] = local;
            bind_shape_variable(variable.name, local, false, span);
        }
        if (dimensions) release_owned(builder_,*dimensions, shape_type);
    }
    const auto evaluate = [&](std::size_t axis) {
        const auto& current = plan.axes[axis];
        if (current.check != ShapeAxisCheck::Value || shape.extents[axis]) return;
        const auto local = capture_extent(*current.capture, "shape.extent");
        shape.extents[axis] = local;
        if (!current.variable.empty() && current.variable_index < plan.variables.size() &&
            plan.variables[current.variable_index].axis == axis &&
            plan.variables[current.variable_index].value) {
            shape.variables[current.variable_index] = local;
            if (plan.variables[current.variable_index].read)
                bind_shape_variable(current.variable, local, true, span);
        }
    };
    const auto result_axis = [&](std::size_t axis) {
        return !unit.parts.empty() && unit.parts.front().parameter == 0 &&
            axis < unit.parts.front().size;
    };
    for (std::size_t axis = 0; axis < plan.axes.size(); ++axis)
        if (!result_axis(axis) && !plan.axes[axis].after_initializer) evaluate(axis);
    std::vector<bool> before_checks(plan.axes.size());
    std::function<void(std::size_t)> require_axis = [&](std::size_t axis) {
        if (before_checks[axis]) return;
        before_checks[axis] = true;
        for (const auto variable : plan.axes[axis].dependencies)
            require_axis(plan.variables.at(variable).axis);
    };
    for (const auto& part : unit.parts)
        if (part.parameter != 0)
            for (std::size_t axis = part.offset; axis < part.offset + part.size; ++axis)
                require_axis(axis);
    // A derived result may be a dependency of a parameter constraint in the
    // shared scope. Evaluate only those result dependencies before checks.
    for (std::size_t axis = 0; axis < plan.axes.size(); ++axis)
        if (result_axis(axis) && before_checks[axis] &&
            !plan.axes[axis].after_initializer) evaluate(axis);
    for (const auto axis : plan.after_order)
        if (before_checks[axis]) evaluate(axis);
    const auto repeats = [&] {
        for (std::size_t axis = 0; axis < plan.axes.size(); ++axis)
            if (plan.axes[axis].check == ShapeAxisCheck::Variable)
                shape.extents[axis] = shape.variables[plan.axes[axis].variable_index];
    };
    repeats();
    // One combined parameter check after the complete binding pass.
    for (const auto& part : unit.parts) {
        if (part.parameter == 0) continue;
        std::vector<std::optional<ValueId>> extents;
        std::vector<std::string> names;
        for (std::size_t axis = part.offset; axis < part.offset + part.size; ++axis) {
            const auto position = axis - part.offset;
            const bool proven = proofs && part.parameter <= proofs->size() &&
                position < (*proofs)[part.parameter - 1].axes.size() &&
                (*proofs)[part.parameter - 1].axes[position];
            if (proven || plan.axes[axis].check == ShapeAxisCheck::None || !shape.extents[axis]) {
                extents.push_back(std::nullopt);
            } else {
                const auto value = builder_.fresh();
                builder_.emit(LoadLocal{value, *shape.extents[axis], int64});
                extents.push_back(value);
            }
            names.push_back(plan.axes[axis].variable);
        }
        const bool proven_rank = proofs && part.parameter <= proofs->size() &&
            (*proofs)[part.parameter - 1].rank;
        if (!proven_rank || std::any_of(extents.begin(), extents.end(),
            [](const auto& extent) { return extent.has_value(); })) {
            const auto current = builder_.fresh();
            if (scope_.is_source_reference(part.subject)) {
                builder_.emit(LoadReference{current, scope_.source_reference(part.subject),
                    scope_.reference_type_at(scope_.source_reference(part.subject))});
            } else {
                builder_.emit(LoadLocal{current, scope_.source_local(part.subject),
                    scope_.source_local_type(part.subject)});
            }
            builder_.emit(ShapedConstraintCheck{current, TypeKind::Tensor,
                std::move(extents), {line, column}, std::move(names), part.subject, part.ellipsis});
        }
    }
    // Derived result extents are captured after argument checks.
    for (std::size_t axis = 0; axis < plan.axes.size(); ++axis)
        if (result_axis(axis) && !plan.axes[axis].after_initializer) evaluate(axis);
    for (const auto axis : plan.after_order)
        if (result_axis(axis)) evaluate(axis);
    repeats();
    std::vector<CapturedExtents> parts;
    for (const auto& part : unit.parts) {
        parts.emplace_back(shape.extents.begin() + part.offset,
                           shape.extents.begin() + part.offset + part.size);
        parts.back().ellipsis = part.ellipsis;
    }
    return parts;
}

void ShapeConstraintLowering::lower_case_shape(
    const ShapePlan& plan, ValueId value, TypeKind kind, const std::string& next,
    SourceSpan span) {
    (void)kind;
    const auto line = static_cast<std::uint32_t>(span.start.line);
    const auto column = static_cast<std::uint32_t>(span.start.column);
    const auto int64 = Type::simple(TypeKind::Int64);
    const auto rank = plan.axes.size();

    // The expressions over no variable of the shape, evaluated when the
    // case is tried; a case without tests evaluates only the constant
    // `nat N = EXPR` variables its body reads.
    BraceShape shape;
    if (plan.conditional) {
        shape = capture_brace_shape(plan, span, false);
    } else {
        shape.extents.resize(rank);
        shape.variables.resize(plan.variables.size());
        for (std::size_t variable = 0; variable < plan.variables.size(); ++variable) {
            const auto& current = plan.variables[variable];
            if (!current.read || !current.value) continue;
            shape.variables[variable] = capture_extent(*plan.axes[current.axis].capture, "shape.extent");
        }
    }

    std::vector<bool> needed(rank, false);
    for (const auto& variable : plan.variables) {
        if (variable.read && !variable.value) needed[variable.axis] = true;
    }
    if (plan.conditional) {
        for (std::size_t axis = 0; axis < rank; ++axis)
            if (plan.axes[axis].check != ShapeAxisCheck::None) needed[axis] = true;
    }
    if (std::none_of(needed.begin(), needed.end(), [](bool b) { return b; }) &&
        !(plan.conditional && plan.rank_unknown)) {
        for (std::size_t variable = 0; variable < plan.variables.size(); ++variable) {
            const auto& current = plan.variables[variable];
            if (current.read && shape.variables[variable])
                bind_shape_variable(current.name, *shape.variables[variable], true, span);
        }
        return;
    }

    const auto shape_type = Type::array(int64, plan.ellipsis || (plan.conditional && plan.rank_unknown) ? -1 : static_cast<long long>(rank));
    auto extents = builder_.fresh();
    builder_.emit(TensorShape{extents, value, shape_type});
    if (plan.conditional && plan.rank_unknown) {
        auto count = builder_.fresh();
        builder_.emit(ArrayLength{count, extents});
        auto wrong = builder_.fresh();
        builder_.emit(Binary{
            wrong, plan.ellipsis ? "<" : "!=", count, builder_.const_int(static_cast<long long>(rank)), int64,
            Type::simple(TypeKind::Bool), line, column});
        const auto release = builder_.label("shape.case.release");
        const auto read = builder_.label("shape.case.read");
        builder_.emit(Branch{wrong, release, read});
        builder_.enter(builder_.add_block(release));
        release_owned(builder_,extents, shape_type);
        builder_.emit(Jump{next});
        builder_.enter(builder_.add_block(read));
    }
    std::vector<std::optional<ValueId>> axis_extents(rank);
    for (std::size_t axis = 0; axis < rank; ++axis) {
        if (!needed[axis]) continue;
        auto extent = builder_.fresh();
        builder_.emit(ArrayGet{
            extent, extents, shape_axis(extents, axis, rank, plan.ellipsis, span), int64, {line,
            column}, true, true});
        axis_extents[axis] = extent;
    }
    release_owned(builder_,extents, shape_type);

    for (std::size_t variable = 0; variable < plan.variables.size(); ++variable) {
        const auto& current = plan.variables[variable];
        if (!current.read || current.value) continue;
        const auto local = builder_.hidden("shape.extent");
        scope_.local_type(local) = int64;
        builder_.emit(DeclareLocal{local, int64, {}, 0, 0});
        builder_.emit(StoreLocal{local, *axis_extents[current.axis], int64});
        shape.variables[variable] = local;
    }
    if (plan.conditional) {
        for (const auto axis : plan.after_order) {
            const auto& current = plan.axes[axis];
            const auto local = capture_extent(*current.capture, "shape.extent");
            shape.extents[axis] = local;
            if (!current.variable.empty() && current.variable_index < plan.variables.size() &&
                plan.variables[current.variable_index].axis == axis &&
                plan.variables[current.variable_index].value) {
                shape.variables[current.variable_index] = local;
            }
        }
        // Every constraint of the case takes part in its selection.
        for (std::size_t axis = 0; axis < rank; ++axis) {
            const auto& current = plan.axes[axis];
            std::optional<std::string> expected;
            if (current.check == ShapeAxisCheck::Value) expected = shape.extents[axis];
            else if (current.check == ShapeAxisCheck::Variable)
                expected = shape.variables[current.variable_index];
            if (!expected) continue;
            auto wanted = builder_.fresh();
            builder_.emit(LoadLocal{wanted, *expected, int64});
            auto differs = builder_.fresh();
            builder_.emit(Binary{
                differs, "!=", *axis_extents[axis], wanted, int64,
                Type::simple(TypeKind::Bool), line, column});
            const auto holds = builder_.label("shape.case.holds");
            builder_.emit(Branch{differs, next, holds});
            builder_.enter(builder_.add_block(holds));
        }
    }
    // The case is taken: its variables.
    for (std::size_t variable = 0; variable < plan.variables.size(); ++variable) {
        const auto& current = plan.variables[variable];
        if (!current.read || !shape.variables[variable]) continue;
        bind_shape_variable(current.name, *shape.variables[variable],
                            current.value != nullptr, span);
    }
}

std::vector<std::optional<ValueId>> ShapeConstraintLowering::load_captured_extents(
    const std::vector<std::optional<std::string>>& captured) {
    std::vector<std::optional<ValueId>> values;
    values.reserve(captured.size());
    for (const auto& name : captured) {
        if (!name) {
            values.push_back(std::nullopt);
            continue;
        }
        auto value = builder_.fresh();
        builder_.emit(
            LoadLocal{value, *name, Type::simple(TypeKind::Int64)});
        values.push_back(value);
    }
    return values;
}

void ShapeConstraintLowering::emit_shaped_constraint(
    ValueId value, TypeKind kind,
    const CapturedExtents& captured,
    SourceSpan span, const Expr* expression) {
    if (captured.empty()) return;
    auto remaining = captured;
    for (std::size_t axis = 0; axis < remaining.size(); ++axis)
        if (proof_.axis(expression, axis)) remaining[axis].reset();
    if (proof_.rank(expression) && std::none_of(remaining.begin(), remaining.end(),
        [](const auto& extent) { return extent.has_value(); })) return;
    builder_.emit(ShapedConstraintCheck{
        value, kind, load_captured_extents(remaining),
        {static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)}, {}, {}, captured.ellipsis});
}

bool ShapeConstraintLowering::deeper_array_constraint(
    const std::vector<std::optional<std::string>>& captured,
    std::size_t axis) const {
    for (std::size_t i = axis; i < captured.size(); ++i) {
        if (captured[i]) return true;
    }
    return false;
}

void ShapeConstraintLowering::emit_array_constraints(
    ValueId array, const Type& array_type,
    const std::vector<std::optional<std::string>>& captured,
    std::size_t axis, SourceSpan span, const Expr* expression) {
    if (axis >= captured.size() || array_type.kind != TypeKind::Array) return;

    if (captured[axis] && !proof_.axis(expression, axis)) {
        auto actual = builder_.fresh();
        builder_.emit(ArrayLength{actual, array});
        auto expected = builder_.fresh();
        builder_.emit(
            LoadLocal{expected, *captured[axis], Type::simple(TypeKind::Int64)});
        builder_.emit(ExtentEqualCheck{
            actual, expected,
            {static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column)}});
    }

    if (axis + 1 >= captured.size() || !array_type.first ||
        array_type.first->kind != TypeKind::Array ||
        !deeper_array_constraint(captured, axis + 1)) {
        return;
    }

    bool remaining = false;
    for (std::size_t i = axis + 1; i < captured.size(); ++i)
        remaining = remaining || (captured[i] && !proof_.axis(expression, i));
    if (!remaining) return;
    auto count = builder_.fresh();
    builder_.emit(ArrayLength{count, array});
    const auto index_name = builder_.hidden("shape.index");
    scope_.local_type(index_name) = Type::simple(TypeKind::Int64);
    builder_.emit(
        DeclareLocal{index_name, Type::simple(TypeKind::Int64), {}, 0, 0});
    builder_.emit(
        StoreLocal{index_name, builder_.const_int(0), Type::simple(TypeKind::Int64)});

    const auto cond = builder_.label("shape.cond");
    const auto body = builder_.label("shape.body");
    const auto done = builder_.label("shape.done");
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(cond));
    auto index = builder_.fresh();
    builder_.emit(
        LoadLocal{index, index_name, Type::simple(TypeKind::Int64)});
    auto cmp = builder_.fresh();
    builder_.emit(Binary{
        cmp, "<", index, count, Type::simple(TypeKind::Int64),
        Type::simple(TypeKind::Bool),
        static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)});
    builder_.emit(Branch{cmp, body, done});

    builder_.enter(builder_.add_block(body));
    auto child = builder_.fresh();
    builder_.emit(ArrayGet{
        child, array, index, *array_type.first,
        {static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)},
        false, true});
    emit_array_constraints(
        child, *array_type.first, captured, axis + 1, span, expression);
    auto next = builder_.fresh();
    builder_.emit(Binary{
        next, "+", index, builder_.const_int(1), Type::simple(TypeKind::Int64),
        Type::simple(TypeKind::Int64),
        static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)});
    builder_.emit(
        StoreLocal{index_name, next, Type::simple(TypeKind::Int64)});
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(done));
}

ValueId ShapeConstraintLowering::generated_shape_array(
    const std::vector<std::optional<std::string>>& captured,
    SourceSpan span) {
    const auto shape_type = Type::array(Type::simple(TypeKind::Int64));
    auto storage = builder_.fresh();
    auto length = builder_.const_int(static_cast<long long>(captured.size()));
    builder_.emit(
        ArrayAlloc{storage, length, shape_type, false});
    for (std::size_t i = 0; i < captured.size(); ++i) {
        if (!captured[i]) {
            throw std::logic_error(
                "contextual tensor allocation contains an unconstrained axis");
        }
        auto extent = builder_.fresh();
        builder_.emit(
            LoadLocal{extent, *captured[i], Type::simple(TypeKind::Int64)});
        builder_.emit(ArraySet{
            storage, builder_.const_int(static_cast<long long>(i)), extent,
            Type::simple(TypeKind::Int64),
            {static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column)},
            false, true});
    }
    return storage;
}

} // namespace quidra::lowering

namespace quidra::lowering {
void ShapeConstraintLowering::initialize_captured_extents(
    const std::vector<std::shared_ptr<Expr>>& expressions,
    const std::vector<std::optional<std::string>>& captured) {
    if(expressions.size()!=captured.size()) throw std::logic_error("parallel shape capture mismatch");
    for(std::size_t i=0;i<expressions.size();++i) if(expressions[i] && captured[i]) {
        const auto found=checked_.extent_capture_operands.find(expressions[i].get());
        const auto* operand=found==checked_.extent_capture_operands.end()?expressions[i].get():found->second;
        const auto value=extent_value(*operand);
        builder_.emit(StoreLocal{*captured[i],value,Type::simple(TypeKind::Int64)});
    }
}
}
