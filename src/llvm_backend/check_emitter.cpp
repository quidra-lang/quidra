#include "llvm_backend/check_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/numeric_types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void CheckEmitter::emit(const ir::ShapedConstraintCheck& n,const ir::Instruction&){
    const auto rank = static_cast<long long>(n.extents.size());
    builder_.call(runtime_abi::tensor::rank_check,{{ptr,symbols_.value(n.value)},{i64,n.ellipsis ? -rank - 1 : rank},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    for(std::size_t axis=0;axis<n.extents.size();++axis){
        if(!n.extents[axis]) continue;
        const auto runtime_axis = n.ellipsis && axis >= *n.ellipsis
            ? -static_cast<long long>(n.extents.size() - axis)
            : static_cast<long long>(axis);
        // An axis that a shape variable fixes names the variable and the
        // binding when it fails (L17).
        if(axis<n.shape_variables.size()&&!n.shape_variables[axis].empty()){
            const auto variable=pool_.intern(n.shape_variables[axis]);
            const auto subject=pool_.intern(n.subject);
            builder_.call(runtime_abi::shape_variable_support::check,{{ptr,symbols_.value(n.value)},{i64,runtime_axis},{i64,symbols_.value(*n.extents[axis])},{ptr,"@"+variable},{ptr,"@"+subject},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            continue;
        }
        builder_.call(runtime_abi::tensor::extent_check,{{ptr,symbols_.value(n.value)},{i64,runtime_axis},{i64,symbols_.value(*n.extents[axis])},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }
}

void CheckEmitter::emit(const ir::ExtentEqualCheck& n,const ir::Instruction&){
    const auto bad=names_.value("extent.mismatch");
    builder_.icmp(bad,IntPredicate::ne,i64,symbols_.value(n.actual),symbols_.value(n.expected));
    if(n.for_collection_lengths) {
        const auto reason=abi::FailureReason::for_lengths_mismatch;
        const auto code=pool_.intern(std::string(abi::spelling(abi::failure_reason_info(reason).code)));
        const auto message=pool_.intern(abi::format(reason,{}));
        fail_fast_.fail_if(bad,"@"+code,"@"+message,"for.length",n.position.line,n.position.column);
    } else fail_fast_.fail_if(bad,"@.code.shape","@.msg.shape","shape.extent",n.position.line,n.position.column);
}

void CheckEmitter::emit(const ir::TestAssert& n,const ir::Instruction&){
    builder_.call(runtime_abi::check::test_assert,{{i1,symbols_.value(n.condition)}});
}

void CheckEmitter::emit(const ir::ForStepCheck& n,const ir::Instruction&){
    builder_.call(runtime_abi::check::for_step,
        {{i64,symbols_.value(n.step)},{i1,n.descending?1:0},
         {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
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
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"for.iteration",n.position.line,n.position.column);
}

void CheckEmitter::emit(const ir::TraversalSiteCheck& n,const ir::Instruction&) {
    const auto bad=names_.value("traversal.moved");
    builder_.icmp(bad,IntPredicate::ne,ptr,symbols_.value(n.expected),symbols_.value(n.current));
    const auto& reason=abi::failure_reason_info(abi::FailureReason::iterated_array_reshaped);
    const auto code=pool_.intern(std::string(abi::spelling(reason.code)));
    const auto message=pool_.intern(std::string(reason.message));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"for.traversal",n.position.line,n.position.column);
}

void CheckEmitter::emit(const ir::SelfReferenceCheck& n,const ir::Instruction&){
    const auto equal=names_.value("reference.same.root"),nonzero=names_.value("reference.root.nonzero"),bad=names_.value("reference.interior");
    builder_.icmp(equal,IntPredicate::eq,i64,symbols_.value(n.holder_root),symbols_.value(n.target_root));
    builder_.icmp(nonzero,IntPredicate::ne,i64,symbols_.value(n.target_root),0);
    builder_.binary(bad,BinaryOp::and_,i1,equal,nonzero);
    const auto reason=abi::FailureReason::reference_interior;
    const auto code=pool_.intern(std::string(abi::spelling(abi::failure_reason_info(reason).code)));
    const auto message=pool_.intern(abi::format(reason,{.path=n.holder_path}));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"reference.self",n.position.line,n.position.column);
}
void CheckEmitter::emit(const ir::StoredTargetsCheck& n,const ir::Instruction&){
    const auto bad=names_.value("reference.stored.interior");
    const auto helper=stored_targets_name(n.type);
    builder_.call(LlvmCall{i1,helper}.result(bad).argument({llvm_type(n.type),symbols_.value(n.value)}).argument({i64,symbols_.value(n.destination_root)}));
    const auto reason=abi::FailureReason::reference_interior;
    const auto code=pool_.intern(std::string(abi::spelling(abi::failure_reason_info(reason).code)));
    const auto message=pool_.intern(abi::format(reason,{.path=n.destination_path}));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"reference.store",n.position.line,n.position.column);
}

void CheckEmitter::emit(const ir::ReferenceBoundCheck& n,const ir::Instruction&){
    const auto bad=names_.value("reference.unbound");
    builder_.icmp(bad,IntPredicate::eq,ptr,symbols_.value(n.address),"null");
    const auto reason=abi::FailureReason::reference_unbound;
    const auto code=pool_.intern(std::string(abi::spelling(abi::failure_reason_info(reason).code)));
    const auto message=pool_.intern(abi::format(reason,{.path=n.path}));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"reference.check",n.position.line,n.position.column);
}

void CheckEmitter::emit(const ir::InitializedCheck& n,const ir::Instruction&){
    const auto bad=names_.value("init.unset");
    builder_.icmp(bad,IntPredicate::eq,i1,symbols_.value(n.flag),0);
    const auto reason=abi::FailureReason::path_uninitialized;
    const auto code=pool_.intern(std::string(abi::spelling(abi::failure_reason_info(reason).code)));
    const auto message=pool_.intern(abi::format(reason,{.path=n.path}));
    fail_fast_.fail_if(bad,"@"+code,"@"+message,"init.check",n.position.line,n.position.column);
}

namespace {

abi::RealOperation real_operation(ir::FiniteCause cause){
    switch(cause){
        case ir::FiniteCause::addition: return abi::RealOperation::addition;
        case ir::FiniteCause::subtraction: return abi::RealOperation::subtraction;
        case ir::FiniteCause::multiplication: return abi::RealOperation::multiplication;
        case ir::FiniteCause::division: return abi::RealOperation::division;
        case ir::FiniteCause::power: break;
    }
    return abi::RealOperation::power;
}

// The dtype code of a fixed-width real type, as quidra_runtime_real_fail
// takes it.
int real_type_code(const Type& type){
    return abi::dtype_code(*numeric_kind_info(type.kind)->dtype);
}

} // namespace

std::string CheckEmitter::non_finite(const ir::CheckedReal& value){
    if(value.type.kind==TypeKind::Real16b){
        // bfloat16 bits: the exponent all ones.
        const auto exponent=names_.value("finite.exponent"),bad=names_.value("finite.bad");
        builder_.binary(exponent,BinaryOp::and_,i16,symbols_.value(value.value),32640);
        builder_.icmp(bad,IntPredicate::eq,i16,exponent,32640);
        return bad;
    }
    // x - x is 0 for a finite x and NaN for NaN and the infinities.
    const auto type=llvm_type(value.type);
    const auto difference=names_.value("finite.difference"),bad=names_.value("finite.bad");
    builder_.binary(difference,BinaryOp::fsub,type,symbols_.value(value.value),symbols_.value(value.value));
    builder_.fcmp(bad,FloatPredicate::uno,type,difference,difference);
    return bad;
}

void CheckEmitter::report(const ir::CheckedReal& value){
    std::string zero_divisor="false";
    if(value.cause==ir::FiniteCause::division&&value.type.kind==TypeKind::Real16b){
        // ±0 has no bit set but the sign.
        const auto magnitude=names_.value("finite.magnitude");
        zero_divisor=names_.value("finite.zero");
        builder_.binary(magnitude,BinaryOp::and_,i16,symbols_.value(value.right),32767);
        builder_.icmp(zero_divisor,IntPredicate::eq,i16,magnitude,0);
    }else if(value.cause==ir::FiniteCause::division){
        zero_divisor=names_.value("finite.zero");
        builder_.fcmp(zero_divisor,FloatPredicate::oeq,llvm_type(value.type),symbols_.value(value.right),"0.0");
    }
    builder_.call(runtime_abi::finite_support::real_fail,
                  {{i32,real_type_code(value.type)},{i32,static_cast<int>(real_operation(value.cause))},
                   {i1,zero_divisor},{i64,sites_.line(value.position.line)},{i64,sites_.column(value.position.column)}});
    builder_.unreachable();
}

void CheckEmitter::emit(const ir::FiniteCheck& n,const ir::Instruction&){
    // The hot path tests the primary values; the covered ones are finite
    // when they are.
    std::string bad;
    for(const auto& value:n.primary){
        const auto tested=non_finite(value);
        if(bad.empty()){bad=tested;continue;}
        const auto either=names_.value("finite.any");
        builder_.binary(either,BinaryOp::or_,i1,bad,tested);
        bad=either;
    }
    const auto fail=names_.label("finite.fail"),ok=names_.label("finite.ok");
    builder_.br(bad,fail,ok);
    builder_.block(fail);
    // The first non-finite value in program order: the covered values, then
    // the primary ones; the last is non-finite when no other is.
    std::vector<const ir::CheckedReal*> order;
    for(const auto& value:n.covered) order.push_back(&value);
    for(const auto& value:n.primary) order.push_back(&value);
    for(std::size_t i=0;i+1<order.size();++i){
        const auto tested=non_finite(*order[i]);
        const auto here=names_.label("finite.report"),next=names_.label("finite.next");
        builder_.br(tested,here,next);
        builder_.block(here);
        report(*order[i]);
        builder_.block(next);
    }
    report(*order.back());
    builder_.block(ok);
}

void CheckEmitter::emit(const ir::FiniteFlagUpdate& n,const ir::Instruction&){
    // Branch-free: the flag ORs in the value's test.
    const auto bad=non_finite(n.value);
    const auto old=names_.value("finite.flag"),updated=names_.value("finite.flag.any");
    builder_.load(old,i1,symbols_.local(n.flag),Align::none);
    builder_.binary(updated,BinaryOp::or_,i1,old,bad);
    builder_.store({i1,updated},symbols_.local(n.flag),Align::none);
}

void CheckEmitter::emit(const ir::FiniteFlagCheck& n,const ir::Instruction&){
    const auto bad=names_.value("finite.flag");
    builder_.load(bad,i1,symbols_.local(n.flag),Align::none);
    const auto fail=names_.label("finite.fail"),ok=names_.label("finite.ok");
    builder_.br(bad,fail,ok);
    builder_.block(fail);
    report(n.value);
    builder_.block(ok);
}

} // namespace quidra::llvm_backend
