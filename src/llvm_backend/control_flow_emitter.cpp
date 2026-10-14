#include "llvm_backend/control_flow_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"

#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void ControlFlowEmitter::emit(const ir::Exit& n,const ir::Instruction&){
    auto s="%exit.status."+std::to_string(n.status);
    builder_.cast(s,CastOp::trunc,{i64,symbols_.value(n.status)},i32);
    lifetime_.cleanup_owned_values();
    builder_.call(runtime_abi::program::process_exit,{{i32,s}});
    builder_.unreachable();
}

void ControlFlowEmitter::emit(const ir::FailError& n,const ir::Instruction&){
    builder_.call(runtime_abi::failure::fail_error,{{ptr,symbols_.value(n.error)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    builder_.unreachable();
}

void ControlFlowEmitter::emit(const ir::Return& n,const ir::Instruction&){
    symbols_.transfer_owned(n.value,n.type);
    lifetime_.cleanup_owned_values();
    if(guard_stack_depth_)builder_.call(runtime_abi::prelude::stack_leave,{});
    if(fn_.entrypoint)builder_.call(runtime_abi::program::set_source_provenance,{{ptr,"null"}});
    if(tuple_indirect_result(n.type)) {
        builder_.store({llvm_type(n.type),symbols_.value(n.value)},"%quidra.result",Align::eight);
        builder_.ret_void();
    } else if(n.type.kind==TypeKind::Void)builder_.ret_void();
    else builder_.ret({llvm_type(n.type),symbols_.value(n.value)});
}

void ControlFlowEmitter::emit(const ir::ReturnVoid&,const ir::Instruction&){
    lifetime_.cleanup_owned_values();
    if(guard_stack_depth_)builder_.call(runtime_abi::prelude::stack_leave,{});
    if(fn_.entrypoint)builder_.call(runtime_abi::program::set_source_provenance,{{ptr,"null"}});
    builder_.ret_void();
}

void ControlFlowEmitter::emit(const ir::Jump& n,const ir::Instruction&){
    builder_.br(n.target);
}

void ControlFlowEmitter::emit(const ir::Branch& n,const ir::Instruction&){
    builder_.br(symbols_.value(n.condition),n.if_true,n.if_false);
}

} // namespace quidra::llvm_backend
