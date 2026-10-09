#pragma once

// ModuleContext: what ModuleEmitter::analyze finds out about the module
// before any function is emitted; read-only afterwards. Every FunctionEmitter
// reads the functions a call can name (for their parameters), the class
// layouts, the C symbols of externs, the array layout policy and the
// recursive functions (those that reach themselves through direct calls).
// ModuleEmitter derives each function's call-depth flags from the recursive
// functions, the functions whose address is taken and the self-depth
// recursive functions (recursive functions that count their depth in a
// parameter; none with debug info). It also holds what decides where a
// failure is reported (StatementAttribution): the user files with their
// index, and how each function a call can name relates to the user's
// statement.
//
// The containers are filled in module order and only looked up afterwards;
// none of them is copied.

#include "quidra/ir/module.hpp"
#include "llvm_backend/storage_layout.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace quidra::llvm_backend {

// The module's source table (@.quidra.sources, abi::SourceTable), which the
// entry registers before its first statement.
inline constexpr std::string_view source_table_symbol = ".quidra.sources";

// What a direct call's callee is, for the user's statement: a function of a
// user file (its statements set the user statement themselves), a function
// of package code (it may fail at the user's statement), or a C function,
// which may fail there too and, given an `fn` argument, may call user code.
enum class CallTarget { user_function, package_function, foreign, foreign_with_callback };

struct ModuleContext {
    // The user files (ir::Module::user_sources) by absolute path, with their
    // index.
    std::unordered_map<std::string, std::size_t> user_source_index;
    // Every function a call can name, by its callee symbol (without '@').
    std::unordered_map<std::string, CallTarget> call_targets;
    // Every function but the entry point, by name; it points into the module.
    std::unordered_map<std::string, const ir::Function*> callable_functions;
    std::unordered_map<std::string, ir::ClassLayout> layouts;
    std::unordered_map<std::string,std::string> external_symbols;
    ArrayLayoutPolicy array_layout;
    std::unordered_set<std::string> recursive_functions;
    std::unordered_set<std::string> address_taken_functions;
    std::unordered_set<std::string> self_depth_recursive_functions;
};

} // namespace quidra::llvm_backend
