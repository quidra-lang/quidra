#include "llvm_backend/concurrency_emitter.hpp"

#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "quidra/standard_classes.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void ConcurrencyEmitter::emit(const ir::AtomicCounterCreate& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::atomic_counter));
    builder_.call(symbols_.value(n.out),runtime_abi::concurrency::atomic_counter_create,{{i64,symbols_.value(n.initial)}});
}

void ConcurrencyEmitter::emit(const ir::AtomicCounterAdd& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.call(symbols_.value(n.out),runtime_abi::concurrency::atomic_counter_add,{{ptr,symbols_.value(n.counter)},{i64,symbols_.value(n.delta)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void ConcurrencyEmitter::emit(const ir::AtomicCounterLoad& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.call(symbols_.value(n.out),runtime_abi::concurrency::atomic_counter_load,{{ptr,symbols_.value(n.counter)}});
}

void ConcurrencyEmitter::emit(const ir::TaskAll& n,const ir::Instruction&){
    if(n.shared!=0){
        builder_.call(runtime_abi::concurrency::task_all_atomic_counter,{{ptr,symbols_.value(n.operations)},{ptr,symbols_.value(n.shared)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(n.result_type.kind==TypeKind::Void){
        builder_.call(runtime_abi::concurrency::task_all,{{ptr,symbols_.value(n.operations)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        const auto count=names_.value("task.all.count");
        symbols_.set_type(n.out,Type::array(n.result_type));
        builder_.load(count,i64,symbols_.value(n.operations),Align::one);
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::array_alloc,{{i64,count},{i64,8},{i32,1}});
        if(is_bare_integer(n.result_type)){
            builder_.call(void_type,LlvmOperand::global(int_helper::task_all),{{ptr,symbols_.value(n.operations)},{ptr,symbols_.value(n.out)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
            return;
        }
        const auto& function=n.result_type.kind==TypeKind::Int64
            ? runtime_abi::concurrency::task_all_i64 : runtime_abi::concurrency::task_all_f64;
        builder_.call(function,{{ptr,symbols_.value(n.operations)},{ptr,symbols_.value(n.out)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

} // namespace quidra::llvm_backend
