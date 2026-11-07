#pragma once

#include "quidra/ast.hpp"

namespace quidra {

// Range bounds are executable expressions, separate from the candidate's
// initializer or storage. Keep metadata walkers from overlooking their calls.
template<class Visit>
void visit_range_bounds(const std::optional<RangeConstraint>& range, const Visit& visit) {
    if (!range) return;
    if (range->upper_first) {
        if (range->upper) visit(*range->upper);
        if (range->lower) visit(*range->lower);
    } else {
        if (range->lower) visit(*range->lower);
        if (range->upper) visit(*range->upper);
    }
}

template<class Visit>
void visit_type_range_bounds(const TypeName& type, const Visit& visit) {
    for (const auto& axis : type.shape_axes) visit_range_bounds(axis.range_constraint, visit);
    for (const auto& argument : type.arguments) visit_type_range_bounds(argument, visit);
    for (const auto& parameter : type.function_parameters) visit_type_range_bounds(parameter, visit);
}

template<class Visit>
void visit_parameter_range_bounds(const Parameter& parameter, const Visit& visit) {
    visit_range_bounds(parameter.range_constraint, visit);
    visit_type_range_bounds(parameter.type, visit);
}

// Direct statement metadata only; callers retain responsibility for nested
// statements and for the evaluation order relative to the initializer/RHS.
template<class Visit>
void visit_statement_range_bounds(const Stmt& statement, const Visit& visit) {
    std::visit([&](const auto& node) {
        if constexpr (requires { node.range_constraint; })
            visit_range_bounds(node.range_constraint, visit);
        if constexpr (requires { node.declared_type; })
            visit_type_range_bounds(node.declared_type, visit);
        if constexpr (requires { node.pattern; })
            if (node.pattern) visit_pattern(*node.pattern, [&](const Pattern& pattern) {
                if (pattern.type) visit_type_range_bounds(*pattern.type, visit);
            });
    }, statement.data);
}

} // namespace quidra
