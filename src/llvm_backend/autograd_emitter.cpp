#include "llvm_backend/autograd_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "quidra/ir/dtype.hpp"
#include "quidra/standard_classes.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void AutogradEmitter::reserve(const ir::TensorBackward& n,const ir::Instruction& ins){
    if(n.targets.empty()&&n.autograd_targets==0)
        throw std::logic_error("tensor.backward requires an explicit gradient target");
    if(!n.targets.empty()){
        scratch_.reserve(ins,LlvmType::array(n.targets.size(),ptr),Align::none);
        scratch_.reserve(ins,LlvmType::array(n.targets.size(),i8),Align::none);
    }
}

void AutogradEmitter::emit(const ir::TensorIsTracked& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::is_tracked,{{ptr,symbols_.value(n.tensor)}});
}

void AutogradEmitter::emit(const ir::TensorHasGrad& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::has_grad,{{ptr,symbols_.value(n.tensor)}});
}

void AutogradEmitter::emit(const ir::TensorClearGrad& n,const ir::Instruction&){
    builder_.call(runtime_abi::tensor::clear_grad,{{ptr,symbols_.value(n.tensor)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void AutogradEmitter::emit(const ir::TensorTrack& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.mode==ir::tensor_track_mode::track && n.target!=0){
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::track_target,{{ptr,symbols_.value(n.tensor)},{ptr,symbols_.value(n.target)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        // Every mode other than untrack and track retracks.
        const auto& callee=n.mode==ir::tensor_track_mode::untrack?runtime_abi::tensor::untrack:
                           n.mode==ir::tensor_track_mode::track?runtime_abi::tensor::track:runtime_abi::tensor::retrack;
        builder_.call(symbols_.value(n.out),callee,{{ptr,symbols_.value(n.tensor)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

void AutogradEmitter::emit(const ir::TensorBackward& n,const ir::Instruction& ins){
    if(n.targets.empty()&&n.autograd_targets==0)
        throw std::logic_error("tensor.backward requires an explicit gradient target");
    std::string raw_targets="null";
    std::string kinds="null";
    if(!n.targets.empty()){
        raw_targets=scratch_.instruction_slot(ins,0);
        kinds=scratch_.instruction_slot(ins,1);
        for(std::size_t i=0;i<n.targets.size();++i){
            const auto target_slot=names_.value("tensor.backward.target");
            const auto kind_slot=names_.value("tensor.backward.kind");
            builder_.getelementptr(target_slot,Inbounds::yes,LlvmType::array(n.targets.size(),ptr),raw_targets,{{i64,0},{i64,i}});
            builder_.store({ptr,symbols_.value(n.targets[i].value)},target_slot,Align::none);
            builder_.getelementptr(kind_slot,Inbounds::yes,LlvmType::array(n.targets.size(),i8),kinds,{{i64,0},{i64,i}});
            builder_.store({i8,n.targets[i].autograd_target?1:0},kind_slot,Align::none);
        }
    }
    if(n.autograd_targets!=0){
        builder_.call(runtime_abi::tensor::backward_many_with_autograd_targets,{{ptr,symbols_.value(n.tensor)},{ptr,raw_targets},{ptr,kinds},{i64,n.targets.size()},{ptr,symbols_.value(n.autograd_targets)},{i1,symbols_.value(n.track)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        builder_.call(runtime_abi::tensor::backward_many,{{ptr,symbols_.value(n.tensor)},{ptr,raw_targets},{ptr,kinds},{i64,n.targets.size()},{i1,symbols_.value(n.track)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

void AutogradEmitter::emit(const ir::TensorGrad& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::grad,{{ptr,symbols_.value(n.tensor)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void AutogradEmitter::emit(const ir::AutogradTargetCreate& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::autograd_target));
    builder_.call(symbols_.value(n.out),runtime_abi::autograd::target_create,{});
}

void AutogradEmitter::emit(const ir::AutogradTargetHasGrad& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::autograd::target_has_grad,{{ptr,symbols_.value(n.target)}});
}

void AutogradEmitter::emit(const ir::AutogradTargetClearGrad& n,const ir::Instruction&){
    builder_.call(runtime_abi::autograd::target_clear_grad,{{ptr,symbols_.value(n.target)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void AutogradEmitter::emit(const ir::AutogradTargetGradient& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::autograd::target_gradient,{{ptr,symbols_.value(n.target)},{i32,abi::dtype_code(ir::dtype_of(*n.type.first))},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

} // namespace quidra::llvm_backend
