#include "llvm_backend/text_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/string_codes.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void TextEmitter::reserve(const ir::StringConcat& n,const ir::Instruction& ins){
    scratch_.reserve(ins,LlvmType::array(n.values.size(),ptr),Align::none);
}

void TextEmitter::reserve(const ir::StringBuild& n,const ir::Instruction& ins){
    scratch_.reserve(ins,LlvmType::array(n.parts.size(),i8),Align::none);
    scratch_.reserve(ins,LlvmType::array(n.parts.size(),i64),Align::eight);
}

void TextEmitter::reserve(const ir::StringBuildAppendMove& n,const ir::Instruction& ins){
    scratch_.reserve(ins,LlvmType::array(n.parts.size(),i8),Align::none);
    scratch_.reserve(ins,LlvmType::array(n.parts.size(),i64),Align::eight);
    scratch_.reserve(ins,i64,Align::eight);
}

void TextEmitter::reserve(const ir::StringAppendMove& n,const ir::Instruction& ins){
    scratch_.reserve(ins,LlvmType::array(n.suffixes.size(),ptr),Align::none);
}

void TextEmitter::reserve(const ir::StringParseTwoSigned&,const ir::Instruction& ins){
    scratch_.reserve(ins,i64,Align::eight);
    scratch_.reserve(ins,i64,Align::eight);
}

void TextEmitter::emit(const ir::StringIndex& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::text::index,{{ptr,symbols_.value(n.text)},{i64,symbols_.value(n.index)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TextEmitter::emit(const ir::StringIndexAsciiCompare& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    const auto compared=names_.value("string.index.ascii");
    builder_.call(compared,runtime_abi::text::index_equal_ascii,{{ptr,symbols_.value(n.text)},{i64,symbols_.value(n.index)},{i8,static_cast<unsigned>(n.byte)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    if(n.negate) builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,compared,"true");
    else copy_by_xor_false(builder_,symbols_.value(n.out),compared);
}

void TextEmitter::emit(const ir::StringAsciiCountPrefix& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.call(symbols_.value(n.out),runtime_abi::text::count_ascii_prefix,{{ptr,symbols_.value(n.text)},{i64,symbols_.value(n.count)},{i8,static_cast<unsigned>(n.byte)},{i1,n.negate?"true":"false"},{i64,symbols_.value(n.initial)},{i64,n.index_line},{i64,n.index_column},{i64,n.overflow_line},{i64,n.overflow_column}});
}

void TextEmitter::emit(const ir::StringLength& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.call(symbols_.value(n.out),runtime_abi::text::length,{{ptr,symbols_.value(n.text)}});
}

void TextEmitter::emit(const ir::StringEmpty& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    const auto first=names_.value("string.empty.byte");
    builder_.load(first,i8,symbols_.value(n.text),Align::one);
    builder_.icmp(symbols_.value(n.out),n.negate?IntPredicate::ne:IntPredicate::eq,i8,first,0);
}

void TextEmitter::emit(const ir::StringContains& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::text::contains,{{ptr,symbols_.value(n.text)},{ptr,symbols_.value(n.needle)}});
}

void TextEmitter::emit(const ir::StringStartsWith& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::text::starts_with,{{ptr,symbols_.value(n.text)},{ptr,symbols_.value(n.prefix)}});
}

void TextEmitter::emit(const ir::StringEndsWith& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::text::ends_with,{{ptr,symbols_.value(n.text)},{ptr,symbols_.value(n.suffix)}});
}

void TextEmitter::emit(const ir::StringFind& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("string.find.raw"),found=names_.value("string.find.found"),result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::text::find,{{ptr,symbols_.value(n.text)},{ptr,symbols_.value(n.needle)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(found,IntPredicate::sge,i64,raw,0);
    const auto yes=names_.label("string.find.value"),missing=names_.label("string.find.none"),done=names_.label("string.find.done");
    builder_.br(found,yes,missing);
    builder_.block(yes);
    // The position is a value of the union's integer case: a bare integer's
    // word (a byte position is inline) or the raw int64.
    Type position_type=Type::simple(TypeKind::Int64);
    for(const auto& current:n.result_type.cases)
        if(current.kind!=TypeKind::None) position_type=current;
    union_box_.store_tag(result,case_index(n.result_type,position_type));
    const auto payload=names_.value("string.find.payload");
    union_box_.payload_slot(payload,result);
    std::string position=raw;
    if(is_bare_integer(position_type)){
        position=names_.value("string.find.word");
        builder_.binary(position,BinaryOp::shl,NoWrap::nsw,i64,raw,1);
    }
    builder_.store({i64,position},payload,Align::none);
    builder_.br(done);
    builder_.block(missing);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::None)));
    builder_.br(done);
    builder_.block(done);
}

void TextEmitter::emit(const ir::StringSlice& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::text::slice,{{ptr,symbols_.value(n.text)},{i64,symbols_.value(n.start)},{i64,symbols_.value(n.end)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TextEmitter::emit(const ir::StringTrim& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::text::trim,{{ptr,symbols_.value(n.text)}});
}

void TextEmitter::emit(const ir::StringSplit& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::array(Type::simple(TypeKind::String)));
    builder_.call(symbols_.value(n.out),runtime_abi::text::split,{{ptr,symbols_.value(n.text)},{ptr,symbols_.value(n.separator)}});
}

void TextEmitter::emit(const ir::StringSplitIterBegin& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bin));
    builder_.call(symbols_.value(n.out),n.move_source?runtime_abi::text::split_iter_begin_move:runtime_abi::text::split_iter_begin,{{ptr,symbols_.value(n.text)},{ptr,symbols_.value(n.separator)}});
}

void TextEmitter::emit(const ir::StringSplitIterNext& n,const ir::Instruction&){
    symbols_.set_type(n.text,Type::simple(TypeKind::String));
    symbols_.set_type(n.has_value,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.text),runtime_abi::text::split_iter_next,{{ptr,symbols_.value(n.cursor)}});
    builder_.icmp(symbols_.value(n.has_value),IntPredicate::ne,ptr,symbols_.value(n.text),"null");
}

void TextEmitter::emit(const ir::StringSplitIterEnd& n,const ir::Instruction&){
    builder_.call(runtime_abi::text::split_iter_end,{{ptr,symbols_.value(n.cursor)}});
}

void TextEmitter::emit(const ir::StringParseTwoSigned& n,const ir::Instruction& ins){
    symbols_.set_type(n.left,Type::simple(TypeKind::Int64));
    symbols_.set_type(n.right,Type::simple(TypeKind::Int64));
    symbols_.set_type(n.ok,Type::simple(TypeKind::Bool));
    const auto& left_slot=scratch_.instruction_slot(ins,0);
    const auto& right_slot=scratch_.instruction_slot(ins,1);
    builder_.call(symbols_.value(n.ok),runtime_abi::prelude::string_parse_two_signed,{{ptr,symbols_.value(n.text)},{i8,static_cast<unsigned>(n.separator)},{ptr,left_slot},{ptr,right_slot}});
    builder_.load(symbols_.value(n.left),i64,left_slot,Align::eight);
    builder_.load(symbols_.value(n.right),i64,right_slot,Align::eight);
}

void TextEmitter::emit(const ir::StringUtf8& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bin));
    builder_.call(symbols_.value(n.out),runtime_abi::text::utf8,{{ptr,symbols_.value(n.text)}});
}

void TextEmitter::emit(const ir::StringFromUtf8& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("string.from_utf8.raw");
    const auto ok=names_.value("string.from_utf8.ok");
    const auto result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::bin::try_utf8,{{ptr,symbols_.value(n.bin)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(ok,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label("string.from_utf8.value");
    const auto bad=names_.label("string.from_utf8.error");
    const auto done=names_.label("string.from_utf8.done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::String)));
    const auto value_payload=names_.value("string.from_utf8.value.payload");
    union_box_.payload_slot(value_payload,result);
    builder_.store({ptr,raw},value_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("string.from_utf8.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.utf8"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void TextEmitter::emit(const ir::StringFromUtf8ArrayDirect& n,const ir::Instruction&){
    symbols_.set_type(n.text,Type::simple(TypeKind::String));
    symbols_.set_type(n.ok,Type::simple(TypeKind::Bool));
    symbols_.set_type(n.error,Type::simple(TypeKind::Error));
    builder_.call(symbols_.value(n.text),runtime_abi::text::u8_array_try_utf8,{{ptr,symbols_.value(n.array)}});
    builder_.icmp(symbols_.value(n.ok),IntPredicate::ne,ptr,symbols_.value(n.text),"null");
    copy_by_zero_gep(builder_,symbols_.value(n.error),Inbounds::yes,"@.err.utf8");
}

void TextEmitter::emit(const ir::StringCodepoints& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::array(Type::simple(TypeKind::Int)));
    builder_.call(symbols_.value(n.out),runtime_abi::text::codepoints,{{ptr,symbols_.value(n.text)}});
}

void TextEmitter::emit(const ir::StringJoin& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::text::join,{{ptr,symbols_.value(n.values)},{ptr,symbols_.value(n.separator)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TextEmitter::emit(const ir::StringConcat& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    const auto& items=scratch_.instruction_slot(ins);
    for(std::size_t i=0;i<n.values.size();++i){
        const auto slot=names_.value("string.concat.slot");
        builder_.getelementptr(slot,Inbounds::yes,LlvmType::array(n.values.size(),ptr),items,{{i64,0},{i64,i}});
        builder_.store({ptr,symbols_.value(n.values[i])},slot,Align::none);
    }
    builder_.call(symbols_.value(n.out),runtime_abi::text::concat_many,{{ptr,items},{i64,n.values.size()}});
}

// The runtime entry a string build writes its parts for (StringBuildTarget):
// the prefix of the names, whether the entry takes a one-byte ASCII string
// as a part kind of its own (abi::string_build_part_kind::ascii_character),
// and the message of a part of a type the
// entry cannot take.
struct StringBuildTarget {
    const char* prefix;
    bool single_byte_ascii_kind;
    const char* unsupported_part;
};

namespace {

constexpr StringBuildTarget string_build{"string.build",false,"unsupported typed string builder part"};
constexpr StringBuildTarget string_build_append{"string.build.append",true,"unsupported typed string build-append part"};

} // namespace

std::string TextEmitter::emit_build_part(const StringBuildTarget& target,const ir::StringBuildPart& part,std::size_t index,
                                  std::size_t count,const std::string& kinds,const std::string& raw_values){
    const std::string prefix=target.prefix;
    unsigned kind=0;
    std::string bits;
    if(is_bare_integer(part.type)){
        // An inline word is a signed integer part; a boxed one becomes the
        // text of its value, released after the build.
        const auto kind_slot=names_.value(prefix+".kind");
        const auto value_slot=names_.value(prefix+".value");
        builder_.getelementptr(kind_slot,Inbounds::yes,LlvmType::array(count,i8),kinds,{{i64,0},{i64,index}});
        builder_.getelementptr(value_slot,Inbounds::yes,LlvmType::array(count,i64),raw_values,{{i64,0},{i64,index}});
        const auto text=names_.value(prefix+".int.text");
        builder_.call(text,ptr,LlvmOperand::global(int_helper::build_part),{{bare_integer_type,symbols_.value(part.value)},{ptr,kind_slot},{ptr,value_slot}});
        return text;
    }
    if(part.type.kind==TypeKind::String||part.type.kind==TypeKind::Error){
        kind=target.single_byte_ascii_kind&&part.single_byte_ascii?abi::string_build_part_kind::ascii_character:abi::string_build_part_kind::text;
        bits=names_.value(prefix+".ptr");
        builder_.cast(bits,CastOp::ptrtoint,{ptr,symbols_.value(part.value)},i64);
    }else if(is_fixed_integer(part.type)){
        kind=is_signed_integer(part.type)?abi::string_build_part_kind::signed_integer:abi::string_build_part_kind::unsigned_integer;
        bits=symbols_.value(part.value);
        if(integer_width(part.type)<64){
            const auto widened=names_.value(prefix+".int");
            builder_.cast(widened,is_signed_integer(part.type)?CastOp::sext:CastOp::zext,{llvm_type(part.type),bits},i64);
            bits=widened;
        }
    }else if(part.type.kind==TypeKind::Bool){
        kind=abi::string_build_part_kind::boolean;
        bits=names_.value(prefix+".bool");
        builder_.cast(bits,CastOp::zext,{i1,symbols_.value(part.value)},i64);
    }else{
        throw std::logic_error(target.unsupported_part);
    }
    const auto kind_slot=names_.value(prefix+".kind");
    const auto value_slot=names_.value(prefix+".value");
    builder_.getelementptr(kind_slot,Inbounds::yes,LlvmType::array(count,i8),kinds,{{i64,0},{i64,index}});
    builder_.store({i8,kind},kind_slot,Align::none);
    builder_.getelementptr(value_slot,Inbounds::yes,LlvmType::array(count,i64),raw_values,{{i64,0},{i64,index}});
    builder_.store({i64,bits},value_slot,Align::eight);
    return {};
}

void TextEmitter::emit(const ir::StringBuild& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    const auto& kinds=scratch_.instruction_slot(ins,0);
    const auto& raw_values=scratch_.instruction_slot(ins,1);
    std::vector<std::string> texts;
    for(std::size_t i=0;i<n.parts.size();++i)
        if(auto text=emit_build_part(string_build,n.parts[i],i,n.parts.size(),kinds,raw_values);!text.empty())
            texts.push_back(std::move(text));
    builder_.call(symbols_.value(n.out),runtime_abi::text::build,{{ptr,kinds},{ptr,raw_values},{i64,n.parts.size()},{ptr,symbols_.value(n.separator)}});
    for(const auto& text:texts) builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
}

void TextEmitter::emit(const ir::StringBuildAppendMove& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    symbols_.set_type(n.added_length,Type::simple(TypeKind::Int64));
    const auto& kinds=scratch_.instruction_slot(ins,0);
    const auto& raw_values=scratch_.instruction_slot(ins,1);
    const auto& added_length_slot=scratch_.instruction_slot(ins,2);
    std::vector<std::string> texts;
    for(std::size_t i=0;i<n.parts.size();++i)
        if(auto text=emit_build_part(string_build_append,n.parts[i],i,n.parts.size(),kinds,raw_values);!text.empty())
            texts.push_back(std::move(text));
    builder_.call(symbols_.value(n.out),runtime_abi::text::build_append_move_unique_direct,{{ptr,symbols_.value(n.text)},{ptr,kinds},{ptr,raw_values},{i64,n.parts.size()},{ptr,symbols_.value(n.separator)},{ptr,added_length_slot}});
    builder_.load(symbols_.value(n.added_length),i64,added_length_slot,Align::eight);
    for(const auto& text:texts) builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
}

void TextEmitter::emit(const ir::StringCanAppendMove& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::text::can_append_move,{{ptr,symbols_.value(n.text)}});
}

void TextEmitter::emit(const ir::StringAppendMove& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    const auto& items=scratch_.instruction_slot(ins);
    for(std::size_t i=0;i<n.suffixes.size();++i){
        const auto slot=names_.value("string.append.slot");
        builder_.getelementptr(slot,Inbounds::yes,LlvmType::array(n.suffixes.size(),ptr),items,{{i64,0},{i64,i}});
        builder_.store({ptr,symbols_.value(n.suffixes[i])},slot,Align::none);
    }
    builder_.call(symbols_.value(n.out),runtime_abi::text::append_move_many,{{ptr,symbols_.value(n.text)},{ptr,items},{i64,n.suffixes.size()}});
}

void TextEmitter::emit(const ir::StringRepeat& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::text::repeat,{{i64,symbols_.value(n.count)},{ptr,symbols_.value(n.fill)}});
}

} // namespace quidra::llvm_backend
