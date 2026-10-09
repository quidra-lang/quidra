#pragma once

// JsonEmitter: JSON values of a function, through the runtime: parsing
// text to a value | error result, the kind and size of a value, member and
// element lookup, reading a value as text, an integer, a float, a big
// integer, a big real or a bool (a T | error result), encoding a value as
// text, and structural equality.
//
// A member of an object and an element of an array are found by one
// lookup (JsonLookup, json_emitter.cpp), which differs only in the kind of
// container, the runtime function and the type of its key: a value | none
// result, or an error when the value is not that kind of container. An
// integer, a float and a bool are read by one scalar read (JsonScalarRead):
// the runtime tests the value's kind, then reads it, and the result is
// T | error. Text is read by one function that returns null for any other
// kind, and big numbers by parsing the number's text, so they are not
// scalar reads.
//
// Owns no state. JsonBigInt and JsonBigReal share one body,
// emit_big_number, which differs only in the parse function and the result
// case; their temporaries and labels come from TemporaryNames in the same
// order for both.

#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

struct JsonLookup;
struct JsonScalarRead;

class JsonEmitter {
public:
    JsonEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,TemporaryNames& names,UnionBox& union_box)
        :builder_(builder),symbols_(symbols),names_(names),union_box_(union_box){}

    void emit(const ir::JsonParse& n,const ir::Instruction& ins);
    void emit(const ir::JsonKind& n,const ir::Instruction& ins);
    void emit(const ir::JsonSize& n,const ir::Instruction& ins);
    void emit(const ir::JsonGet& n,const ir::Instruction& ins);
    void emit(const ir::JsonAt& n,const ir::Instruction& ins);
    void emit(const ir::JsonText& n,const ir::Instruction& ins);
    void emit(const ir::JsonInteger& n,const ir::Instruction& ins);
    void emit(const ir::JsonNumber& n,const ir::Instruction& ins);
    void emit(const ir::JsonBigInt& n,const ir::Instruction& ins);
    void emit(const ir::JsonBigReal& n,const ir::Instruction& ins);
    void emit(const ir::JsonBoolean& n,const ir::Instruction& ins);
    void emit(const ir::JsonEncode& n,const ir::Instruction& ins);
    void emit(const ir::JsonEqual& n,const ir::Instruction& ins);

private:
    void emit_lookup(const JsonLookup& lookup,ir::ValueId result_value,ir::ValueId json,ir::ValueId key,const Type& result_type);
    void emit_scalar_read(const JsonScalarRead& read,ir::ValueId result_value,ir::ValueId json,const Type& result_type);
    template <class T> void emit_big_number(const T& n);

    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    UnionBox& union_box_;
};

} // namespace quidra::llvm_backend
