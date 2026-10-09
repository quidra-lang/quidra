#pragma once

// HttpEmitter: HTTP requests of a function, through the runtime: a GET
// request gives a $std.http.Response | error result, whose error carries
// the runtime's last error message; a response header gives a
// string | none result.
//
// Owns no state.

#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class HttpEmitter {
public:
    HttpEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,TemporaryNames& names,UnionBox& union_box)
        :builder_(builder),symbols_(symbols),names_(names),union_box_(union_box){}

    void emit(const ir::HttpGet& n,const ir::Instruction& ins);
    void emit(const ir::HttpHeader& n,const ir::Instruction& ins);

private:
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    UnionBox& union_box_;
};

} // namespace quidra::llvm_backend
