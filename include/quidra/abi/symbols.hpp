#pragma once

// Symbol names that the generated program, the runtime and native packages
// agree on.
//
// Owns: the symbol namespaces of a program, the symbol of a user-defined
// function ("n_" + the sanitized Quidra name), and the start of the names the
// frontend gives the instances of generic declarations. The checker reserves
// every namespace for the generated program and the runtime: an extern
// declaration may not name a symbol in one (checker.cpp, reserved symbol
// names).

#include <cctype>
#include <string>
#include <string_view>

namespace quidra::abi {

// Who defines which symbols of a program.
namespace symbol_namespace {
// The program's entry point, which generated code defines.
inline constexpr std::string_view entry = "main";
// The prefix of the user-defined functions (user_symbol).
inline constexpr std::string_view user_prefix = "n_";
// The prefix of the runtime's entry points (abi/runtime_entry_points.hpp)
// and of the helpers generated code defines for itself.
inline constexpr std::string_view runtime_prefix = "quidra_";
// The prefix of the names the compiler makes up: the generated module's
// internal helpers, and the instances the frontend creates from generic
// declarations (generic_instance_prefix).
inline constexpr std::string_view internal_prefix = "__quidra_";
// The prefix of the runtime's HTTP entry points; a program that calls one
// links libcurl.
inline constexpr std::string_view http_prefix = "quidra_http_";
} // namespace symbol_namespace

// `name` as symbol text, appended to `out`: every character outside
// [A-Za-z0-9_] replaced by '_'.
inline void append_symbol_text(std::string& out, std::string_view name) {
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
}

// The LLVM symbol of the user-defined function `name`: "n_", then the symbol
// text of the name.
inline std::string user_symbol(std::string name){std::string out(symbol_namespace::user_prefix);append_symbol_text(out,name);return out;}

// The kind of instance in the name of an instance of a generic class. It is
// named because the standard classes' instance prefixes are built from it
// (standard_classes.hpp).
inline constexpr std::string_view generic_class_kind = "gc";

// The start of the name the frontend gives an instance of the generic
// declaration `name`: internal_prefix, the kind of instance, '_', the
// symbol text of the name and '_'. The frontend appends a hash of the
// instance's type arguments (frontend.cpp, mangle).
inline std::string generic_instance_prefix(std::string_view kind, std::string_view name) {
    std::string out(symbol_namespace::internal_prefix);
    out += kind;
    out += '_';
    append_symbol_text(out, name);
    out += '_';
    return out;
}

} // namespace quidra::abi
