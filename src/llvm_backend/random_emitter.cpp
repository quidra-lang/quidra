#include "llvm_backend/random_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "quidra/standard_classes.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void RandomEmitter::emit(const ir::RandomGenerator& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::random_generator));
    builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,8}});
    builder_.store({i64,symbols_.value(n.seed)},symbols_.value(n.out),Align::one);
}

void RandomEmitter::emit(const ir::RandomInt& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    const auto valid=names_.value("random.int.valid"),bad=names_.value("random.int.bad");
    builder_.icmp(valid,IntPredicate::slt,i64,symbols_.value(n.start),symbols_.value(n.end));
    builder_.binary(bad,BinaryOp::xor_,i1,valid,"true");
    fail_fast_.fail_if(bad,"@.code.random.range","@.msg.random.range","random.int",n.line,n.column);
    builder_.call(symbols_.value(n.out),runtime_abi::random::int_value,{{ptr,symbols_.value(n.generator)},{i64,symbols_.value(n.start)},{i64,symbols_.value(n.end)}});
}

void RandomEmitter::emit(const ir::RandomFloat& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Real64));
    builder_.call(symbols_.value(n.out),runtime_abi::random::float_value,{{ptr,symbols_.value(n.generator)}});
}

void RandomEmitter::emit(const ir::RandomBool& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::random::bool_value,{{ptr,symbols_.value(n.generator)}});
}

} // namespace quidra::llvm_backend
