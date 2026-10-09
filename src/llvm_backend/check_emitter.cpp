#include "llvm_backend/check_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <cstddef>
#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void CheckEmitter::emit(const ir::ShapedConstraintCheck& n,const ir::Instruction&){
    builder_.call(runtime_abi::tensor::rank_check,{{ptr,symbols_.value(n.value)},{i64,n.extents.size()},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    for(std::size_t axis=0;axis<n.extents.size();++axis){
        if(!n.extents[axis]) continue;
        builder_.call(runtime_abi::tensor::extent_check,{{ptr,symbols_.value(n.value)},{i64,axis},{i64,symbols_.value(*n.extents[axis])},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

void CheckEmitter::emit(const ir::ExtentEqualCheck& n,const ir::Instruction&){
    const auto bad=names_.value("extent.mismatch");
    builder_.icmp(bad,IntPredicate::ne,i64,symbols_.value(n.actual),symbols_.value(n.expected));
    fail_fast_.fail_if(bad,"@.code.shape","@.msg.shape","shape.extent",n.line,n.column);
}

void CheckEmitter::emit(const ir::TestAssert& n,const ir::Instruction&){
    builder_.call(runtime_abi::check::test_assert,{{i1,symbols_.value(n.condition)}});
}

void CheckEmitter::emit(const ir::RangeCheckStep& n,const ir::Instruction&){
    auto z="%range.zero."+std::to_string(n.step),bad="range.bad."+std::to_string(n.step),ok="range.ok."+std::to_string(n.step);
    builder_.icmp(z,IntPredicate::eq,i64,symbols_.value(n.step),0);
    builder_.br(z,bad,ok);
    builder_.block(bad);
    builder_.call(runtime_abi::prelude::fail_at,{{ptr,"@.code.range.step"},{ptr,"@.msg.range.step"},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    builder_.unreachable();
    builder_.block(ok);
}

void CheckEmitter::emit(const ir::IterationShapeCheck& n,const ir::Instruction&){
    const auto& type=symbols_.type_at(n.array);
    const auto moved=names_.value("iteration.moved");
    builder_.icmp(moved,IntPredicate::ne,ptr,symbols_.value(n.array),symbols_.value(n.entry_array));
    auto bad=moved;
    if(!is_fixed_array(type)){
        // The length of a dynamic array or a bin is its first word.
        const auto length=names_.value("iteration.length"),resized=names_.value("iteration.resized");
        bad=names_.value("iteration.bad");
        builder_.load(length,i64,symbols_.value(n.array),
                      type.kind==TypeKind::Bin?Align::one:Align::none);
        builder_.icmp(resized,IntPredicate::ne,i64,length,symbols_.value(n.entry_length));
        builder_.binary(bad,BinaryOp::or_,i1,moved,resized);
    }
    const auto& reason=abi::failure_reason_info(abi::FailureReason::iterated_array_reshaped);
    const auto code=pool_.intern(std::string(abi::spelling(reason.code)));
    const auto message=pool_.intern(std::string(reason.message));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"for.iteration",n.line,n.column);
}

void CheckEmitter::emit(const ir::InitializedCheck& n,const ir::Instruction&){
    const auto bad=names_.value("init.unset");
    builder_.icmp(bad,IntPredicate::eq,i1,symbols_.value(n.flag),0);
    const auto reason=abi::FailureReason::path_uninitialized;
    const auto code=pool_.intern(std::string(abi::spelling(abi::failure_reason_info(reason).code)));
    const auto message=pool_.intern(abi::format(reason,{.path=n.path}));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"init.check",n.line,n.column);
}

} // namespace quidra::llvm_backend
