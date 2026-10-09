#pragma once

// The call graph of a module: which functions can reach themselves through
// direct calls.
//
// Owns: the recursion sets the LLVM backend's call-depth guard is keyed on.
// Later interprocedural analyses (strongly connected components) belong here.
// Only direct calls (ir::Call) are edges.
//
// The module's roots are the entry point and every exported function
// (ir::Function::c_export_symbol), which C code calls from outside the
// module. The sets below consider every function of the module, so they
// already hold for the roots; a pass that removes unreferenced functions or
// changes a function's symbol must keep each root and its symbol (it may
// still inline an exported function's body into its Quidra callers).

#include <string>
#include <unordered_set>

namespace quidra::ir {
struct Module;
} // namespace quidra::ir

namespace quidra::ir::analysis {

// The functions of the module that reach themselves through direct calls to
// functions of the module.
std::unordered_set<std::string> recursive_functions(const ir::Module& module);
// The recursive functions that may keep their call depth in a parameter: they
// call themselves directly, make no indirect call, are not address-taken, and
// neither reach nor are reached by another recursive function (which would
// have to share the global depth counter).
std::unordered_set<std::string> self_depth_recursive_functions(
    const ir::Module& module,
    const std::unordered_set<std::string>& recursive,
    const std::unordered_set<std::string>& address_taken);

} // namespace quidra::ir::analysis
