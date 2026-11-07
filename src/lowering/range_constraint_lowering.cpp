// Ordered numeric preconditions use the same checked common-type conversion
// as scalar comparisons. The candidate has already been converted to its
// destination type; neither its initializer nor a guarded store is repeated.
#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ir::ValueId Lowerer::lower_range_predicate(
    const RangeConstraint& range, ir::ValueId candidate, const Type& type) {
    const auto boolean = Type::simple(TypeKind::Bool);
    const auto position = SourcePosition{
        static_cast<std::uint32_t>(range.span.start.line),
        static_cast<std::uint32_t>(range.span.start.column)};
    const auto compare = [&](const Expr& bound, bool lower, bool inclusive) {
        const auto boundary_type = type_of(checked_, bound);
        const auto common = *numeric_common_type(type, boundary_type);
        const auto boundary = lower_into(bound, boundary_type);
        const auto value = type == common ? candidate
            : conversions_.convert_numeric_argument(candidate, type, common, range.span);
        const auto converted_boundary = boundary_type == common ? boundary
            : conversions_.convert_numeric_argument(boundary, boundary_type, common, bound.span);
        const auto result = builder_.fresh();
        builder_.emit(ir::Binary{result, inclusive ? "<=" : "<",
            lower ? converted_boundary : value, lower ? value : converted_boundary,
            common, boolean, position.line, position.column});
        if (type != common) release_owned(builder_, value, common);
        if (boundary_type != common) release_owned(builder_, converted_boundary, common);
        release_owned(builder_, boundary, boundary_type);
        return result;
    };
    if (!range.lower) return compare(*range.upper, false, range.upper_inclusive);
    if (!range.upper) return compare(*range.lower, true, range.lower_inclusive);
    const auto left = range.upper_first
        ? compare(*range.upper, false, range.upper_inclusive)
        : compare(*range.lower, true, range.lower_inclusive);
    const auto result_name = builder_.hidden("range.result");
    scope_.local_type(result_name) = boolean;
    builder_.emit(ir::DeclareLocal{result_name, boolean, {}, 0, 0});
    builder_.emit(ir::StoreLocal{result_name, left, boolean});
    const auto upper = builder_.label("range.upper");
    const auto done = builder_.label("range.done");
    builder_.emit(ir::Branch{left, upper, done});
    builder_.enter(builder_.add_block(upper));
    const auto right = range.upper_first
        ? compare(*range.lower, true, range.lower_inclusive)
        : compare(*range.upper, false, range.upper_inclusive);
    builder_.emit(ir::StoreLocal{result_name, right, boolean});
    builder_.emit(ir::Jump{done});
    builder_.enter(builder_.add_block(done));
    const auto result = builder_.fresh();
    builder_.emit(ir::LoadLocal{result, result_name, boolean});
    return result;
}

void Lowerer::lower_range_constraint(
    const RangeConstraint& range, ir::ValueId candidate, const Type& type,
    const std::string& subject) {
    // Bounds can replace a parameter's managed storage. Hold its candidate
    // in an owned local so propagated-error exits also clean up the snapshot.
    auto snapshot = candidate;
    std::string held;
    if (owns_managed(type)) {
        snapshot = copy_owned(builder_, candidate, type);
        held = builder_.hidden("range.candidate");
        scope_.local_type(held) = type;
        builder_.emit(ir::DeclareLocal{held, type, {}, 0, 0});
        builder_.emit(ir::StoreLocal{held, snapshot, type});
    }
    const auto release_snapshot = [&] {
        if (held.empty()) return;
        const auto consumed = builder_.fresh();
        builder_.emit(ir::LoadLocal{consumed, held, type, true});
        release_owned(builder_, consumed, type);
    };
    const auto condition = lower_range_predicate(range, snapshot, type);
    const auto passed = builder_.label("range.valid");
    const auto failed = builder_.label("range.invalid");
    builder_.emit(ir::Branch{condition, passed, failed});
    builder_.enter(builder_.add_block(failed));
    const auto problem = builder_.fresh();
    const auto position = SourcePosition{
        static_cast<std::uint32_t>(range.span.start.line),
        static_cast<std::uint32_t>(range.span.start.column)};
    std::string description = "its declared range";
    if (const auto source = checked_.program.source_texts.find(builder_.function()->source_file);
        source != checked_.program.source_texts.end() &&
        range.span.start.offset < range.span.end.offset &&
        range.span.end.offset <= source->second.size()) {
        description = source->second.substr(range.span.start.offset,
                                            range.span.end.offset - range.span.start.offset);
        for (auto& character : description)
            if (character == '\n' || character == '\r') character = ' ';
    }
    const auto string = Type::simple(TypeKind::String);
    const auto prefix = builder_.fresh();
    builder_.emit(ir::ConstantString{prefix,
        "Range constraint '" + description + "' failed for '" + subject + "': value="});
    const auto displayed = builder_.fresh();
    builder_.emit(ir::ToString{displayed, snapshot, type, std::nullopt, false});
    const auto separator = builder_.fresh();
    builder_.emit(ir::ConstantString{separator, ""});
    const auto message = builder_.fresh();
    builder_.emit(ir::StringBuild{message, {{prefix, string}, {displayed, string}}, separator});
    builder_.emit(ir::ErrorMake{problem, 0, "RANGE_CONSTRAINT",
        {{message, {}, false}}, std::nullopt, position});
    release_owned(builder_, prefix, string);
    release_owned(builder_, displayed, string);
    release_owned(builder_, separator, string);
    release_owned(builder_, message, string);
    release_snapshot();
    builder_.emit(ir::FailError{problem, position});
    builder_.enter(builder_.add_block(passed));
    release_snapshot();
}

} // namespace quidra::lowering
