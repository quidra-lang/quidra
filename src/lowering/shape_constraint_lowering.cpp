// Shape constraints: the extents a declaration, parameter or return type
// spells (Tensor[n, m], T[n][m]) are evaluated once into hidden Int
// locals, then checked against the value at binding (ShapedConstraintCheck,
// and ExtentEqualCheck over each nested array level), or used to shape a
// tensor allocation from its context.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <stdexcept>

namespace quidra::lowering {

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
        static_cast<std::uint32_t>(expression.span.start.line),
        static_cast<std::uint32_t>(expression.span.start.column)});
    return out;
}

std::vector<std::optional<std::string>> ShapeConstraintLowering::capture_extents(
    const std::vector<std::shared_ptr<Expr>>& expressions,
    std::string_view prefix) {
    std::vector<std::optional<std::string>> captured;
    captured.reserve(expressions.size());
    for (const auto& expression : expressions) {
        if (!expression) {
            captured.push_back(std::nullopt);
            continue;
        }
        const auto name = builder_.hidden(prefix);
        scope_.local_type(name) = Type::simple(TypeKind::Int64);
        builder_.emit(
            DeclareLocal{name, Type::simple(TypeKind::Int64), {}, 0, 0});
        const auto value = extent_value(*expression);
        builder_.emit(
            StoreLocal{name, value, Type::simple(TypeKind::Int64)});
        captured.push_back(name);
    }
    return captured;
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
    const std::vector<std::optional<std::string>>& captured,
    SourceSpan span) {
    if (captured.empty()) return;
    builder_.emit(ShapedConstraintCheck{
        value, kind, load_captured_extents(captured),
        static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)});
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
    std::size_t axis, SourceSpan span) {
    if (axis >= captured.size() || array_type.kind != TypeKind::Array) return;

    if (captured[axis]) {
        auto actual = builder_.fresh();
        builder_.emit(ArrayLength{actual, array});
        auto expected = builder_.fresh();
        builder_.emit(
            LoadLocal{expected, *captured[axis], Type::simple(TypeKind::Int64)});
        builder_.emit(ExtentEqualCheck{
            actual, expected,
            static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column)});
    }

    if (axis + 1 >= captured.size() || !array_type.first ||
        array_type.first->kind != TypeKind::Array ||
        !deeper_array_constraint(captured, axis + 1)) {
        return;
    }

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
        static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column),
        false, true});
    emit_array_constraints(
        child, *array_type.first, captured, axis + 1, span);
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
            static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column),
            false, true});
    }
    return storage;
}

} // namespace quidra::lowering
