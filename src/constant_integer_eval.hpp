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
        if (!names || name->this_qualifier) return std::nullopt;
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
        if (!target || !is_fixed_integer(*target)) return std::nullopt;
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

// The evaluation of an integer literal expression in the type its literals
// materialize as (a fixed-width integer type or nat), with every
// intermediate result checked against that type's range, as the program
// would compute it. Known: the value. OutOfRange: some operation leaves the
// range while its operands are known and in range. Unknown: not a literal
// expression of +, - and *, or beyond the 64-bit evaluation budget (the
// program then computes it).
struct IntegerInType {
    enum class State { Unknown, Known, OutOfRange } state{State::Unknown};
    long long value{};
};

inline bool integer_fits_literal_type(long long value, const Type& type) {
    if (type.kind == TypeKind::Nat) return value >= 0;
    return integer_value_fits(value, type);
}

inline IntegerInType integer_in_type(const Expr& expression, const Type& type) {
    using State = IntegerInType::State;
    if (type.kind != TypeKind::Nat && !is_fixed_integer(type)) return {};
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        if (!literal->fits_u64 ||
            literal->value > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            return {};
        const auto value = static_cast<long long>(literal->value);
        if (!integer_fits_literal_type(value, type)) return {State::OutOfRange, 0};
        return {State::Known, value};
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        if (unary->op != "-") return {};
        // A negated literal is one value (a signed minimum such as -128 in
        // int8 has no positive counterpart in the type).
        if (const auto* literal = std::get_if<IntegerExpr>(&unary->operand->data)) {
            if (!literal->fits_u64 ||
                literal->value > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
                return {};
            const auto value = -static_cast<long long>(literal->value);
            if (!integer_fits_literal_type(value, type)) return {State::OutOfRange, 0};
            return {State::Known, value};
        }
        const auto operand = integer_in_type(*unary->operand, type);
        if (operand.state != State::Known) return operand;
        if (operand.value == std::numeric_limits<long long>::min()) return {};
        if (!integer_fits_literal_type(-operand.value, type)) return {State::OutOfRange, 0};
        return {State::Known, -operand.value};
    }
    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary || (binary->op != "+" && binary->op != "-" && binary->op != "*")) return {};
    const auto left = integer_in_type(*binary->left, type);
    if (left.state != State::Known) return left;
    const auto right = integer_in_type(*binary->right, type);
    if (right.state != State::Known) return right;
    // The checked 64-bit operation of integer(): a result beyond int64 is
    // outside every signed type and every unsigned type narrower than 64
    // bits; for nat64 and nat it is beyond the evaluation budget.
    const auto evaluated = [&]() -> std::optional<long long> {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        const auto a = left.value, b = right.value;
        if (binary->op == "+") {
            if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return std::nullopt;
            return a + b;
        }
        if (binary->op == "-") {
            if ((b > 0 && a < min + b) || (b < 0 && a > max + b)) return std::nullopt;
            return a - b;
        }
        if (a == 0 || b == 0) return 0LL;
        if (a > 0) {
            if ((b > 0 && a > max / b) || (b < 0 && b < min / a)) return std::nullopt;
        } else {
            if ((b > 0 && a < min / b) || (b < 0 && a < max / b)) return std::nullopt;
        }
        return a * b;
    }();
    if (!evaluated)
        return is_signed_integer(type) || (is_fixed_integer(type) && integer_width(type) < 64)
            ? IntegerInType{State::OutOfRange, 0} : IntegerInType{};
    const auto result = *evaluated;
    if (!integer_fits_literal_type(result, type)) return {State::OutOfRange, 0};
    return {State::Known, result};
}

} // namespace quidra::constant_eval
