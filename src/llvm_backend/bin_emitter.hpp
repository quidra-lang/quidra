#pragma once

// BinEmitter: the bit strings (bin) of a function: allocation with a fill
// bit, length, reading one bit as a one-bit bin, writing one, and slicing,
// each through the runtime or the length word at offset 0. Element and slice
// failures report the instruction's position (the index operand of an
// element access), through the statement attribution's immediates.
//
// Owns no state.

#include "quidra/ir/module.hpp"
#include "llvm_backend/statement_attribution.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class BinEmitter {
public:
    BinEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,const StatementAttribution& sites)
        :builder_(builder),symbols_(symbols),sites_(sites){}

    void emit(const ir::BinAlloc& n,const ir::Instruction& ins);
    void emit(const ir::BinLength& n,const ir::Instruction& ins);
    void emit(const ir::BinGet& n,const ir::Instruction& ins);
    void emit(const ir::BinSet& n,const ir::Instruction& ins);
    void emit(const ir::BinSlice& n,const ir::Instruction& ins);

private:
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
