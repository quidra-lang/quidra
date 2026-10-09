#pragma once

// TextEmitter: the strings of a function: indexing, length, search,
// slicing, trimming, splitting (eagerly or through a split iterator),
// UTF-8 and code point conversion, joining, concatenation, repetition, the
// typed string builder (StringBuild: strings, integers and bools written in
// one runtime call) and the in-place appends of a uniquely owned string
// (StringAppendMove, StringBuildAppendMove), plus the recognized idioms the
// lowering emits (an ASCII byte comparison at an index, an ASCII prefix
// count, a two-number parse).
//
// A part of a typed string build (emit_build_part) is a kind code (string,
// signed or unsigned integer, bool) and 64 bits: the string's address or
// the integer or bool widened to i64, written into the build's two scratch
// arrays. StringBuild and StringBuildAppendMove write their parts the same
// way for their runtime entries (StringBuildTarget); only the append entry
// takes a one-byte ASCII string as a kind of its own.
//
// Owns no state. Concatenation, the builder, the appends and the two-number
// parse pass their operands through the entry-frame scratch slots the
// function's pre-pass reserved for the instruction, so a source loop does
// not grow the stack.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <cstddef>
#include <string>

namespace quidra::llvm_backend {

struct StringBuildTarget;

class TextEmitter {
public:
    TextEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,ScratchPlanner& scratch,TemporaryNames& names,UnionBox& union_box,const StatementAttribution& sites)
        :builder_(builder),symbols_(symbols),scratch_(scratch),names_(names),union_box_(union_box),sites_(sites){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::StringConcat& n,const ir::Instruction& ins);
    void reserve(const ir::StringBuild& n,const ir::Instruction& ins);
    void reserve(const ir::StringBuildAppendMove& n,const ir::Instruction& ins);
    void reserve(const ir::StringAppendMove& n,const ir::Instruction& ins);
    void reserve(const ir::StringParseTwoSigned& n,const ir::Instruction& ins);

    void emit(const ir::StringIndex& n,const ir::Instruction& ins);
    void emit(const ir::StringIndexAsciiCompare& n,const ir::Instruction& ins);
    void emit(const ir::StringAsciiCountPrefix& n,const ir::Instruction& ins);
    void emit(const ir::StringLength& n,const ir::Instruction& ins);
    void emit(const ir::StringEmpty& n,const ir::Instruction& ins);
    void emit(const ir::StringContains& n,const ir::Instruction& ins);
    void emit(const ir::StringStartsWith& n,const ir::Instruction& ins);
    void emit(const ir::StringEndsWith& n,const ir::Instruction& ins);
    void emit(const ir::StringFind& n,const ir::Instruction& ins);
    void emit(const ir::StringSlice& n,const ir::Instruction& ins);
    void emit(const ir::StringTrim& n,const ir::Instruction& ins);
    void emit(const ir::StringSplit& n,const ir::Instruction& ins);
    void emit(const ir::StringSplitIterBegin& n,const ir::Instruction& ins);
    void emit(const ir::StringSplitIterNext& n,const ir::Instruction& ins);
    void emit(const ir::StringSplitIterEnd& n,const ir::Instruction& ins);
    void emit(const ir::StringParseTwoSigned& n,const ir::Instruction& ins);
    void emit(const ir::StringUtf8& n,const ir::Instruction& ins);
    void emit(const ir::StringFromUtf8& n,const ir::Instruction& ins);
    void emit(const ir::StringFromUtf8ArrayDirect& n,const ir::Instruction& ins);
    void emit(const ir::StringCodepoints& n,const ir::Instruction& ins);
    void emit(const ir::StringJoin& n,const ir::Instruction& ins);
    void emit(const ir::StringConcat& n,const ir::Instruction& ins);
    void emit(const ir::StringBuild& n,const ir::Instruction& ins);
    void emit(const ir::StringBuildAppendMove& n,const ir::Instruction& ins);
    void emit(const ir::StringCanAppendMove& n,const ir::Instruction& ins);
    void emit(const ir::StringAppendMove& n,const ir::Instruction& ins);
    void emit(const ir::StringRepeat& n,const ir::Instruction& ins);

private:
    // Returns the text the part made for a boxed integer, which the caller
    // releases after the build, or an empty name.
    std::string emit_build_part(const StringBuildTarget& target,const ir::StringBuildPart& part,std::size_t index,
                         std::size_t count,const std::string& kinds,const std::string& raw_values);

    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    ScratchPlanner& scratch_;
    TemporaryNames& names_;
    UnionBox& union_box_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
