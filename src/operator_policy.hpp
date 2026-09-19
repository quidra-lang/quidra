#pragma once

#include <string_view>

namespace quidra::operator_policy {

inline bool is_short_circuit(std::string_view op) {
    return op == "and" || op == "or";
}

inline bool is_arithmetic(std::string_view op) {
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

inline bool is_bitwise_logic(std::string_view op) {
    return op == "AND" || op == "OR" || op == "XOR";
}

inline bool is_shift(std::string_view op) {
    return op == "<<" || op == ">>";
}

inline bool is_fixed_width_bitwise_binary(std::string_view op) {
    return is_bitwise_logic(op) || is_shift(op);
}

inline bool participates_in_numeric_literal_family(std::string_view op) {
    return is_arithmetic(op) || is_fixed_width_bitwise_binary(op);
}

} // namespace quidra::operator_policy
