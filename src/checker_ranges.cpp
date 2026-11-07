#include "quidra/checker.hpp"
#include "constant_integer_eval.hpp"
#include "numeric_literal_policy.hpp"

namespace quidra {

void Checker::prove_range_constraint(
    const RangeConstraint& range, const Type& type, const Expr* candidate,
    const std::string& subject) {
    if (!candidate || !is_integer_family_type(type)) return;
    const auto fits = [](long long value, const Type& destination) {
        if (destination.kind == TypeKind::Int) return true;
        if (destination.kind == TypeKind::Nat) return value >= 0;
        return integer_value_fits(value, destination);
    };
    const auto value = constant_eval::integer(*candidate, &const_integer_values_);
    if (!value || !fits(*value, type)) return;
    const auto prove = [&](const std::shared_ptr<Expr>& bound, bool lower, bool inclusive) {
        if (!bound) return;
        // Only literal/constant arithmetic bounds are independent of a
        // caller's scope. A callee parameter with the same spelling as a
        // caller constant must never be substituted here.
        const auto limit = constant_eval::integer(*bound);
        if (!limit) return;
        const auto found = expr_types_.find(bound.get());
        if (found == expr_types_.end() || !is_integer_family_type(found->second)) return;
        const auto common = numeric_common_type(type, found->second);
        if (!common || !fits(*value, *common) || !fits(*limit, *common)) return;
        const bool holds = lower ? (inclusive ? *limit <= *value : *limit < *value)
                                 : (inclusive ? *value <= *limit : *value < *limit);
        if (!holds)
            error("RANGE_CONSTRAINT", "Value " + std::to_string(*value) +
                  " does not satisfy the range constraint for '" + subject + "'.",
                  candidate->span);
    };
    prove(range.lower, true, range.lower_inclusive);
    prove(range.upper, false, range.upper_inclusive);
}

void Checker::check_range_constraint(
    const RangeConstraint& range, const Type& type, const Expr* candidate,
    const std::string& subject) {
    if (type.kind == TypeKind::Invalid) return;
    if (!is_ordered_numeric(type)) {
        error("TYPE_MISMATCH", "A range constraint requires an ordered numeric value, not " +
              type_name(type) + ".", range.span);
        return;
    }
    const auto check = [&](const std::shared_ptr<Expr>& bound) {
        if (!bound) return;
        const auto context = numeric_policy::numeric_literal_context(
            &type, numeric_policy::numeric_literal_family(*bound));
        auto actual = check_expr(*bound, context.type && !context.ambiguous ? &*context.type : nullptr);
        if (actual.kind == TypeKind::Union && actual.union_name.empty() &&
            case_index(actual, Type::simple(TypeKind::Error)) >= 0) {
            std::vector<Type> success;
            for (const auto& item : actual.cases)
                if (item.kind != TypeKind::Error) success.push_back(item);
            if (success.size() == 1 && is_numeric(success.front())) {
                actual = success.front();
                expr_types_[bound.get()] = actual;
                fail_fast_expressions_.insert(bound.get());
            }
        }
        if (actual.kind != TypeKind::Invalid &&
            (!is_ordered_numeric(actual) || !numeric_common_type(type, actual)))
            error("TYPE_MISMATCH", "A range bound must be an ordered numeric scalar.", bound->span);
    };
    if (range.upper_first) { check(range.upper); check(range.lower); }
    else { check(range.lower); check(range.upper); }
    prove_range_constraint(range, type, candidate, subject);
}

void Checker::check_parameter_constraints(
    const FunctionDecl& source, const FunctionType& type, std::size_t offset) {
    for (std::size_t i = 0; i < source.parameters.size(); ++i) {
        const auto& parameter = source.parameters[i];
        if (!parameter.range_constraint || i + offset >= type.parameters.size()) continue;
        const auto& resolved = type.parameters[i + offset];
        if (resolved.writable)
            current_reference_effects_[resolved.name].required.insert("");
        const auto unavailable = this_unavailable_;
        if (source.is_constructor) this_unavailable_ = true;
        try {
            check_range_constraint(*parameter.range_constraint, resolved.type, nullptr, parameter.name);
        } catch (...) { this_unavailable_ = unavailable; throw; }
        this_unavailable_ = unavailable;
    }
}

} // namespace quidra
