#pragma once

// ReplEmitter: the REPL instructions of a function. ReplDisplay prints a
// result between the begin and end markers the REPL reads back, through
// ReplValuePrinter; ReplReplayMode switches the flag that suppresses output
// while earlier submissions are replayed.
//
// Owns no state. ReplDisplay interns the two markers in the module's
// StringPool, begin first, before the printer interns its texts; the array
// index slot the printer loops with is the entry-frame scratch slot the
// function's pre-pass reserved for the display.

#include "quidra/ir/module.hpp"
#include "llvm_backend/repl_value_printer.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class ReplEmitter {
public:
    ReplEmitter(StringPool& pool,llvm_text::LlvmBuilder& builder,const SymbolTable& symbols,ScratchPlanner& scratch,
                ReplValuePrinter& repl_printer)
        :pool_(pool),builder_(builder),symbols_(symbols),scratch_(scratch),repl_printer_(repl_printer){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::ReplDisplay& n,const ir::Instruction& ins);

    void emit(const ir::ReplReplayMode& n,const ir::Instruction& ins);
    void emit(const ir::ReplDisplay& n,const ir::Instruction& ins);

private:
    StringPool& pool_;
    llvm_text::LlvmBuilder& builder_;
    const SymbolTable& symbols_;
    ScratchPlanner& scratch_;
    ReplValuePrinter& repl_printer_;
};

} // namespace quidra::llvm_backend
