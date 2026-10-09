#pragma once

// NumericEmitter: scalar arithmetic and comparison of a function. Unary
// negates, inverts or applies not (elementwise through the runtime for a
// tensor); Binary compares addresses, strings, errors and structured values
// (equality helpers), and computes integer arithmetic with overflow,
// division-by-zero and shift-range checks (none when the lowering proved
// the operation cannot overflow), float arithmetic and comparison, bool
// logic, and big integer and big real arithmetic through the runtime.
// ExactAtom and ExactUnary evaluate the exact-number atoms of a numeric
// provider.
//
// Owns no state. reserve interns the provider names during the function's
// pre-pass (FunctionPrepass). Each check allocates its labels through
// FailFastEmitter after the temporaries of the operation it guards, in a
// fixed order per operator.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/fail_fast_emitter.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"


namespace quidra::llvm_backend {

class NumericEmitter {
public:
    NumericEmitter(StringPool& pool,llvm_text::LlvmBuilder& builder,SymbolTable& symbols,TemporaryNames& names,
                   FailFastEmitter& fail_fast,const StatementAttribution& sites)
        :pool_(pool),builder_(builder),symbols_(symbols),names_(names),fail_fast_(fail_fast),sites_(sites){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::ExactAtom& n,const ir::Instruction& ins);
    void reserve(const ir::ExactUnary& n,const ir::Instruction& ins);

    void emit(const ir::ExactAtom& n,const ir::Instruction& ins);
    void emit(const ir::ExactUnary& n,const ir::Instruction& ins);
    void emit(const ir::Unary& n,const ir::Instruction& ins);
    void emit(const ir::Binary& n,const ir::Instruction& ins);

private:
    StringPool& pool_;
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    FailFastEmitter& fail_fast_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
