#pragma once

#include "quidra/ast.hpp"
#include "quidra/language.hpp"
#include "quidra/types.hpp"
#include "operator_policy.hpp"

#include <optional>

namespace quidra::numeric_policy {

enum class NumericLiteralFamily {
    None,
    Integer,
    Real,
    Mixed
};

inline NumericLiteralFamily numeric_literal_family(const Expr& expression) {
    if (std::holds_alternative<IntegerExpr>(expression.data)) {
        return NumericLiteralFamily::Integer;
    }
    if (std::holds_alternative<FloatExpr>(expression.data)) {
        return NumericLiteralFamily::Real;
    }
    if (const auto* name = std::get_if<NameExpr>(&expression.data);
        name && is_standard_real_constant(name->name)) {
        return NumericLiteralFamily::Real;
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data);
        unary && (unary->op == "-" || unary->op == "NOT")) {
        return numeric_literal_family(*unary->operand);
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&expression.data);
        binary && operator_policy::participates_in_numeric_literal_family(binary->op)) {
        const auto left = numeric_literal_family(*binary->left);
        const auto right = numeric_literal_family(*binary->right);
        if (left == NumericLiteralFamily::None || right == NumericLiteralFamily::None) {
            return NumericLiteralFamily::None;
        }
        if (left == right) return left;
        return NumericLiteralFamily::Mixed;
    }
    return NumericLiteralFamily::None;
}

inline NumericLiteralFamily direct_numeric_literal_family(const Expr& expression) {
    if (std::holds_alternative<IntegerExpr>(expression.data)) {
        return NumericLiteralFamily::Integer;
    }
    if (std::holds_alternative<FloatExpr>(expression.data)) {
        return NumericLiteralFamily::Real;
    }
    if (const auto* name = std::get_if<NameExpr>(&expression.data);
        name && is_standard_real_constant(name->name)) {
        return NumericLiteralFamily::Real;
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data);
        unary && (unary->op == "-" || unary->op == "NOT")) {
        return direct_numeric_literal_family(*unary->operand);
    }
    return NumericLiteralFamily::None;
}

struct NumericLiteralContext {
    std::optional<Type> type;
    bool ambiguous{};
    bool opposite_family{};
};

inline NumericLiteralContext numeric_literal_context(
    const Type* expected, NumericLiteralFamily family) {
    NumericLiteralContext result;
    if (!expected ||
        (family != NumericLiteralFamily::Integer &&
         family != NumericLiteralFamily::Real)) {
        return result;
    }

    const auto matches = [&](const Type& candidate) {
        return family == NumericLiteralFamily::Integer
            ? is_integer_family_type(candidate)
            : is_real(candidate);
    };
    const auto opposite = [&](const Type& candidate) {
        return family == NumericLiteralFamily::Integer
            ? is_real(candidate)
            : is_integer_family_type(candidate);
    };
    const auto consider = [&](const Type& candidate) {
        if (matches(candidate)) {
            if (result.type) result.ambiguous = true;
            else result.type = candidate;
        } else if (opposite(candidate)) {
            result.opposite_family = true;
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
