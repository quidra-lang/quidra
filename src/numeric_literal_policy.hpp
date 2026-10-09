#pragma once

#include "quidra/ast.hpp"
#include "quidra/language.hpp"
#include "quidra/types.hpp"
#include "operator_policy.hpp"

#include <optional>

namespace quidra::numeric_policy {

// The category of a numeric literal expression, which decides the types it
// may materialize into: a non-negative integer literal (`10`) materializes
// as nat* or int*, a negative one (unary `-` applied to an integer literal,
// `-0` included) as int* only, a real literal (`0.5`) as real*, and an
// imaginary literal (`2.0i`) or a complex one (a real and an imaginary
// literal joined by `+` or `-`) as a complex type, of which there is none
// yet. Mixed: an expression joining categories that do not combine (an
// integer and a real literal). None: not a literal expression.
enum class NumericLiteralCategory {
    None,
    NonNegativeInteger,
    NegativeInteger,
    Real,
    Imaginary,
    Complex,
    Mixed
};

inline bool integer_category(NumericLiteralCategory category) {
    return category == NumericLiteralCategory::NonNegativeInteger ||
           category == NumericLiteralCategory::NegativeInteger;
}

// A category that materializes into one numeric family: integer or real.
inline bool single_family_category(NumericLiteralCategory category) {
    return integer_category(category) || category == NumericLiteralCategory::Real;
}

// A category whose literals need a complex type.
inline bool complex_category(NumericLiteralCategory category) {
    return category == NumericLiteralCategory::Imaginary ||
           category == NumericLiteralCategory::Complex;
}

// The category of two literal expressions joined by an operator (or two
// branches of an if-expression): integers join as integers, negative when
// either is; a real and an imaginary literal, or a complex one with either,
// make a complex literal; any other pair is Mixed.
inline NumericLiteralCategory join_numeric_literal_categories(NumericLiteralCategory left,
                                                              NumericLiteralCategory right) {
    using C = NumericLiteralCategory;
    if (left == C::None || right == C::None) return C::None;
    if (left == C::Mixed || right == C::Mixed) return C::Mixed;
    if (integer_category(left) && integer_category(right))
        return left == C::NegativeInteger || right == C::NegativeInteger ? C::NegativeInteger
                                                                        : C::NonNegativeInteger;
    if (left == right) return left;
    const auto complex_part = [](C category) {
        return category == C::Real || complex_category(category);
    };
    if (complex_part(left) && complex_part(right)) return C::Complex;
    return C::Mixed;
}

inline NumericLiteralCategory numeric_literal_family(const Expr& expression) {
    using C = NumericLiteralCategory;
    if (std::holds_alternative<IntegerExpr>(expression.data)) return C::NonNegativeInteger;
    if (std::holds_alternative<RealLiteralExpr>(expression.data)) return C::Real;
    if (std::holds_alternative<ImaginaryLiteralExpr>(expression.data)) return C::Imaginary;
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data);
        unary && (unary->op == "-" || unary->op == "NOT")) {
        const auto operand = numeric_literal_family(*unary->operand);
        if (unary->op == "-" && integer_category(operand)) return C::NegativeInteger;
        return operand;
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&expression.data);
        binary && operator_policy::participates_in_numeric_literal_family(binary->op)) {
        return join_numeric_literal_categories(numeric_literal_family(*binary->left),
                                               numeric_literal_family(*binary->right));
    }
    // An if-expression whose branches are all numeric literals is a literal
    // of their joined category, so the context types it as it types a
    // literal.
    if (const auto* conditional = std::get_if<IfExpr>(&expression.data)) {
        auto category = numeric_literal_family(*conditional->otherwise);
        for (const auto& value : conditional->values)
            category = join_numeric_literal_categories(category, numeric_literal_family(*value));
        return category;
    }
    return C::None;
}

inline NumericLiteralCategory direct_numeric_literal_family(const Expr& expression) {
    using C = NumericLiteralCategory;
    if (std::holds_alternative<IntegerExpr>(expression.data)) return C::NonNegativeInteger;
    if (std::holds_alternative<RealLiteralExpr>(expression.data)) return C::Real;
    if (std::holds_alternative<ImaginaryLiteralExpr>(expression.data)) return C::Imaginary;
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data);
        unary && (unary->op == "-" || unary->op == "NOT")) {
        const auto operand = direct_numeric_literal_family(*unary->operand);
        if (unary->op == "-" && integer_category(operand)) return C::NegativeInteger;
        return operand;
    }
    return C::None;
}

struct NumericLiteralContext {
    std::optional<Type> type;
    bool ambiguous{};
    bool opposite_family{};
    // A negative integer literal met a nat type, which it cannot
    // materialize as (named for the diagnostic).
    std::optional<Type> natural_target;
};

inline NumericLiteralContext numeric_literal_context(
    const Type* expected, NumericLiteralCategory category) {
    NumericLiteralContext result;
    if (!expected || !single_family_category(category)) return result;

    const bool integer = integer_category(category);
    const auto natural = [](const Type& candidate) {
        const auto* numeric = numeric_info(candidate);
        return numeric && numeric->family == NumericFamily::Natural;
    };
    const auto matches = [&](const Type& candidate) {
        if (!integer) return is_real(candidate);
        if (!is_integer_family_type(candidate)) return false;
        return category != NumericLiteralCategory::NegativeInteger || !natural(candidate);
    };
    const auto opposite = [&](const Type& candidate) {
        return integer ? is_real(candidate) : is_integer_family_type(candidate);
    };
    const auto consider = [&](const Type& candidate) {
        if (matches(candidate)) {
            if (result.type) result.ambiguous = true;
            else result.type = candidate;
        } else if (opposite(candidate)) {
            result.opposite_family = true;
        } else if (integer && natural(candidate) && !result.natural_target) {
            result.natural_target = candidate;
        }
    };

    if (expected->kind == TypeKind::Union) {
        for (const auto& candidate : expected->cases) consider(candidate);
    } else {
        consider(*expected);
    }
    return result;
}

} // namespace quidra::numeric_policy
