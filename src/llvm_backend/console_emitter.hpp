#pragma once

// ConsoleEmitter: the console of a function. Print writes a value as text
// (skipped while the REPL replays earlier submissions) and Flush flushes the
// output; both give a void | error result from the stream status. Input
// reads a line into a string | error result.
//
// Owns no state. Input reads through the entry-frame scratch slot the
// function's pre-pass reserved for it.

#include "quidra/ir/module.hpp"
#include "llvm_backend/error_result_emitter.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class ConsoleEmitter {
public:
    ConsoleEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,ScratchPlanner& scratch,TemporaryNames& names,
                   ErrorResultEmitter& errors,UnionBox& union_box)
        :builder_(builder),symbols_(symbols),scratch_(scratch),names_(names),errors_(errors),union_box_(union_box){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::Input& n,const ir::Instruction& ins);

    void emit(const ir::Flush& n,const ir::Instruction& ins);
    void emit(const ir::Print& n,const ir::Instruction& ins);
    void emit(const ir::Input& n,const ir::Instruction& ins);

private:
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    ScratchPlanner& scratch_;
    TemporaryNames& names_;
    ErrorResultEmitter& errors_;
    UnionBox& union_box_;
};

} // namespace quidra::llvm_backend
