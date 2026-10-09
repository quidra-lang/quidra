#pragma once

// ConstantEmitter: the literal values of a function. An integer or float
// constant is materialized as an add or fadd of zero, a bool as an xor with
// false (copy_idioms.hpp), a string as a pointer to its pooled global
// (.str.N), and a big integer or big real literal through the runtime from
// its pooled spelling. Each handler gives its IR value its type
// (SymbolTable).
//
// Owns no state. reserve interns the string literals and exact spellings in
// the module's StringPool during the function's pre-pass (FunctionPrepass),
// which allocates their names in instruction order; the handlers look them
// up, so they allocate no name.

#include "quidra/ir/module.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class ConstantEmitter {
public:
    ConstantEmitter(StringPool& pool,llvm_text::LlvmBuilder& builder,SymbolTable& symbols)
        :pool_(pool),builder_(builder),symbols_(symbols){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::ConstantString& n,const ir::Instruction& ins);
    void reserve(const ir::ConstantExact& n,const ir::Instruction& ins);

    void emit(const ir::ConstantInt& n,const ir::Instruction& ins);
    void emit(const ir::ConstantFloat& n,const ir::Instruction& ins);
    void emit(const ir::ConstantExact& n,const ir::Instruction& ins);
    void emit(const ir::ConstantBool& n,const ir::Instruction& ins);
    void emit(const ir::ConstantString& n,const ir::Instruction& ins);

private:
    StringPool& pool_;
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
};

} // namespace quidra::llvm_backend
