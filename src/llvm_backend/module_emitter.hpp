#pragma once

// ModuleEmitter: the LLVM text of a typed-IR module (quidra::emit_llvm).
//
// Emission has two phases. First every function is emitted on its own
// (emit_functions), then the C entry points of exported functions
// (emit_foreign_exports), into the module's bodies, which interns string
// constants, assigns debug ids and finds out whether any debug variable
// exists; only then can the module header, the prelude, the constants, the
// type helpers and the debug metadata be written. The module text
// (LlvmModule) keeps each section apart and joins them in their order.
//
// Owns: the module context, the string pool, the debug metadata (with debug
// info on), the type helpers (TypeHelperSet) and the module text. The
// phases run in a fixed order, and so do the checks that can throw: an
// external declaration that conflicts with an earlier one throws while the
// functions are emitted, the type helper collectors run after every function.

#include "quidra/ir/module.hpp"
#include "llvm_backend/debug_module.hpp"
#include "llvm_backend/module_context.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_text/llvm_module.hpp"

#include <optional>
#include <string>

namespace quidra::llvm_backend {

class ModuleEmitter {
public:
    ModuleEmitter(const ir::Module& module,bool debug_info)
        :module_(module),debug_info_(debug_info),
         type_helpers_(module,context_.layouts,context_.array_layout){}

    // The phases below, in this order.
    std::string emit();

private:
    void analyze();
    void assign_debug_ids();
    void emit_functions();
    void emit_foreign_exports();
    void collect_array_casts_and_clones();
    void emit_header();
    void emit_prelude();
    void emit_constants();
    void emit_helpers();
    void emit_metadata();

    const ir::Module& module_;
    bool debug_info_{};
    StringPool pool_;
    ModuleContext context_;
    std::optional<DebugModule> debug_;
    TypeHelperSet type_helpers_;
    llvm_text::LlvmModule text_;
};

} // namespace quidra::llvm_backend
