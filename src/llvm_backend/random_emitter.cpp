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
    symbols_.acquire_owned(n.out,symbols_.type_at(n.out),symbols_.value(n.out));
}

void RandomEmitter::emit(const ir::RandomInt& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int));
    builder_.call(symbols_.value(n.out),runtime_abi::random::integer_value,
        {{ptr,symbols_.value(n.generator)},{i64,symbols_.value(n.start)},
         {i64,symbols_.value(n.end)},{i64,n.position.line},{i64,n.position.column}});
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
