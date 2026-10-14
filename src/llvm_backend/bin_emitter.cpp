#include "llvm_backend/bin_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void BinEmitter::emit(const ir::BinAlloc& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bin));
    builder_.call(symbols_.value(n.out),runtime_abi::bin::alloc,{{i64,symbols_.value(n.length)},{i64,symbols_.value(n.fill)}});
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::Bin),symbols_.value(n.out));
}

void BinEmitter::emit(const ir::BinLength& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.load(symbols_.value(n.out),i64,symbols_.value(n.bin),Align::one);
}

void BinEmitter::emit(const ir::BinGet& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bin));
    builder_.call(symbols_.value(n.out),runtime_abi::bin::index,{{ptr,symbols_.value(n.bin)},{i64,symbols_.value(n.index)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::Bin),symbols_.value(n.out));
}

void BinEmitter::emit(const ir::BinSet& n,const ir::Instruction&){
    builder_.call(runtime_abi::bin::set,{{ptr,symbols_.value(n.bin)},{i64,symbols_.value(n.index)},{ptr,symbols_.value(n.value)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
}

void BinEmitter::emit(const ir::BinSlicePlanned& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bin));
    const auto operand=[&](const std::optional<ir::ValueId>& value){
        return value?symbols_.value(*value):std::string("0");
    };
    const auto bin=symbols_.value(n.bin);
    const auto start=operand(n.bounds.start),end=operand(n.bounds.end);
    builder_.call(symbols_.value(n.out),runtime_abi::bin::slice_planned,{
        {ptr,bin},{i64,start},{i32,static_cast<int>(n.bounds.start_form)},
        {i64,end},{i32,static_cast<int>(n.bounds.end_form)},
        {i32,static_cast<int>(n.bounds.direction)},
        {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::Bin),symbols_.value(n.out));
}

void BinEmitter::emit(const ir::BinSlice& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bin));
    builder_.call(symbols_.value(n.out),runtime_abi::bin::slice,{{ptr,symbols_.value(n.bin)},{i64,symbols_.value(n.start)},{i64,symbols_.value(n.end)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::Bin),symbols_.value(n.out));
}

} // namespace quidra::llvm_backend
