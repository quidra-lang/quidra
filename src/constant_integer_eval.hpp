#pragma once

#include "quidra/ast.hpp"
#include "quidra/types.hpp"

#include <limits>
#include <optional>
#include <unordered_map>

namespace quidra::constant_eval {

inline std::optional<long long> integer(
    const Expr& expression,
    const std::unordered_map<std::string, long long>* names = nullptr) {
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        if (literal->value >
            static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
            return std::nullopt;
        }
        return static_cast<long long>(literal->value);
    }

    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (!names) return std::nullopt;
        const auto found = names->find(name->name);
        return found == names->end() ? std::nullopt
                                     : std::optional<long long>{found->second};
    }

    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        if (unary->op != "-") return std::nullopt;
        const auto value = integer(*unary->operand, names);
        if (!value || *value == std::numeric_limits<long long>::min()) {
            return std::nullopt;
        }
        return -*value;
    }

    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        if (call->args.size() != 1 || call->args[0].writable || call->args[0].name) {
            return std::nullopt;
        }
        const auto target = builtin_scalar_type(call->callee);
        if (!target || !is_integer(*target)) return std::nullopt;
        const auto value = integer(*call->args[0].value, names);
        if (!value || !integer_value_fits(*value, *target)) return std::nullopt;
        return value;
    }

    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary) return std::nullopt;
    const auto left = integer(*binary->left, names);
    const auto right = integer(*binary->right, names);
    if (!left || !right) return std::nullopt;

    if ((binary->op == "/" || binary->op == "%") && *right == 0) return std::nullopt;
    if ((binary->op == "/" || binary->op == "%") &&
        *left == std::numeric_limits<long long>::min() && *right == -1) {
        return std::nullopt;
    }
    if (binary->op == "/") return *left / *right;
    if (binary->op == "%") return *left % *right;

    const auto checked_add = [](long long a, long long b, long long& out) {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return false;
        out = a + b;
        return true;
    };
    const auto checked_sub = [](long long a, long long b, long long& out) {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        if ((b > 0 && a < min + b) || (b < 0 && a > max + b)) return false;
        out = a - b;
        return true;
    };
    const auto checked_mul = [](long long a, long long b, long long& out) {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        if (a == 0 || b == 0) {
            out = 0;
            return true;
        }
        if (a > 0) {
            if ((b > 0 && a > max / b) || (b < 0 && b < min / a)) return false;
        } else {
            if ((b > 0 && a < min / b) || (b < 0 && a < max / b)) return false;
        }
        out = a * b;
        return true;
    };

    long long result{};
    if (binary->op == "+") {
        if (!checked_add(*left, *right, result)) return std::nullopt;
    } else if (binary->op == "-") {
        if (!checked_sub(*left, *right, result)) return std::nullopt;
    } else if (binary->op == "*") {
        if (!checked_mul(*left, *right, result)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return result;
}

} // namespace quidra::constant_eval
