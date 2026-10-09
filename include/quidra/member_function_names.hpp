#pragma once

// The names the compiler gives the functions of class members: a method's
// function is "$method.<class>.<method>", and a constructor's is
// "$construct.<class>.<n>", n counting the class's constructors from 0 in
// declaration order. The checker gives the names; the source tools, the
// lowering, the optimizer and the backend find members by them.

#include <cstddef>
#include <string>
#include <string_view>

namespace quidra::member_function_name {

inline constexpr std::string_view method_prefix = "$method.";
inline constexpr std::string_view constructor_prefix = "$construct.";

// "$method.<class_name>.<method_name>"
inline std::string method(std::string_view class_name, std::string_view method_name) {
    std::string name(method_prefix);
    name += class_name;
    name += '.';
    name += method_name;
    return name;
}

// "$construct.<class_name>.<index>"
inline std::string constructor(std::string_view class_name, std::size_t index) {
    std::string name(constructor_prefix);
    name += class_name;
    name += '.';
    name += std::to_string(index);
    return name;
}

} // namespace quidra::member_function_name
