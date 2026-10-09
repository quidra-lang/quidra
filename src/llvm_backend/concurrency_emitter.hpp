#pragma once

// ConcurrencyEmitter: tasks and atomic counters of a function, through the
// runtime: running an array of operations (task.all), with no result, with
// an array of Int or Float results, or sharing one atomic counter; and
// creating, adding to and loading an atomic counter ($std.atomic.Counter).
//
// Owns no state.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class ConcurrencyEmitter {
public:
    ConcurrencyEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,TemporaryNames& names,const StatementAttribution& sites)
        :builder_(builder),symbols_(symbols),names_(names),sites_(sites){}

    void emit(const ir::AtomicCounterCreate& n,const ir::Instruction& ins);
    void emit(const ir::AtomicCounterAdd& n,const ir::Instruction& ins);
    void emit(const ir::AtomicCounterLoad& n,const ir::Instruction& ins);
    void emit(const ir::TaskAll& n,const ir::Instruction& ins);

private:
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
