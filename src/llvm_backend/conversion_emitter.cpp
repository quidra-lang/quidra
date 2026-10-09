#include "llvm_backend/conversion_emitter.hpp"

#include "llvm_backend/array_cast_helpers.hpp"
#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/ir/dtype.hpp"

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

namespace {

// The destination of a conversion as the runtime takes it: a dtype code for
// a fixed-width type, abi::conversion_type_integer or _real for an exact one.
int conversion_type(const Type& type){
    if(type.kind==TypeKind::Int) return abi::conversion_type_integer;
    if(type.kind==TypeKind::Nat) return abi::conversion_type_natural;
    if(type.kind==TypeKind::Real) return abi::conversion_type_real;
    return abi::dtype_code(ir::dtype_of(type));
}

// The innermost element type of an array type.
const Type& array_leaf(const Type& type){
    const Type* leaf=&type;
    while(leaf->kind==TypeKind::Array) leaf=leaf->first.get();
    return *leaf;
}

std::string reason_operand(abi::ConversionReason reason){
    return std::to_string(static_cast<int>(reason));
}

constexpr int subject_code(abi::ConversionSubject subject){
    return static_cast<int>(subject);
}

} // namespace

std::string ConversionEmitter::float32_failure_reason(const std::string& value,const std::string& base){
    // inf - inf is NaN; a finite difference is 0.
    const auto difference=base+".difference",infinite=base+".infinite",reason=base+".reason";
    builder_.binary(difference,BinaryOp::fsub,double_type,value,value);
    builder_.fcmp(infinite,FloatPredicate::uno,double_type,difference,difference);
    builder_.select(reason,infinite,{i32,reason_operand(abi::ConversionReason::non_finite)},
                    {i32,reason_operand(abi::ConversionReason::out_of_range)});
    return reason;
}

std::string ConversionEmitter::recorded_failure_reason(const std::string& base){
    const auto reason=base+".reason";
    builder_.call(reason,runtime_abi::failure::last_conversion_reason,{});
    return reason;
}

std::string ConversionEmitter::fraction_or_recorded_reason(const std::string& value,const std::string& base){
    // A small rational has a nonzero denominator word.
    const auto denominator=base+".denominator",small=base+".small",reason=base+".fraction.reason";
    const auto recorded=recorded_failure_reason(base);
    builder_.extractvalue(denominator,exact_real_type,value,1);
    builder_.icmp(small,IntPredicate::ne,i64,denominator,0);
    builder_.select(reason,small,{i32,reason_operand(abi::ConversionReason::not_integral)},
                    {i32,recorded});
    return reason;
}

void ConversionEmitter::store_conversion_message(const std::string& payload,int target,int subject,
                                                 const std::string& reason){
    const auto message=payload+".message";
    builder_.call(message,runtime_abi::failure::conversion_message,{{i32,target},{i32,subject},{i32,reason}});
    builder_.store({ptr,message},payload,Align::none);
}

void ConversionEmitter::reserve(const ir::FallibleNumericConvert& n,const ir::Instruction&){
    const bool exact_source=is_bare_integer(n.source_type) ||
        n.source_type.kind==TypeKind::Real;
    if(exact_source && (is_fixed_integer(n.target_type) || is_bare_integer(n.target_type)))
        scratch_.reserve_shared(ScratchPlanner::SharedSlot::FallibleExactI64,i64,Align::eight);
    else if(exact_source && n.target_type.kind==TypeKind::Real32)
        scratch_.reserve_shared(ScratchPlanner::SharedSlot::FallibleExactFloat32,float_type,Align::four);
    else if(exact_source && n.target_type.kind==TypeKind::Real64)
        scratch_.reserve_shared(ScratchPlanner::SharedSlot::FallibleExactFloat64,double_type,Align::eight);
}

void ConversionEmitter::reserve(const ir::ParseNumber& n,const ir::Instruction& ins){
    if(is_fixed_integer(n.target_type)||is_bare_integer(n.target_type)) scratch_.reserve(ins,i64,Align::none);
    else if(n.target_type.kind==TypeKind::Real32) scratch_.reserve(ins,float_type,Align::none);
    else if(n.target_type.kind==TypeKind::Real64) scratch_.reserve(ins,double_type,Align::none);
}

void ConversionEmitter::reserve(const ir::ParseNumberDirect& n,const ir::Instruction& ins){
    if(is_fixed_integer(n.target_type)) scratch_.reserve(ins,i64,Align::none);
    else if(n.target_type.kind==TypeKind::Real32) scratch_.reserve(ins,float_type,Align::none);
    else if(n.target_type.kind==TypeKind::Real64) scratch_.reserve(ins,double_type,Align::none);
}

void ConversionEmitter::emit(const ir::ArrayNumericCast& n,const ir::Instruction&){
    const bool fallible =
        n.result_type.kind==TypeKind::Union && n.result_type.union_name.empty() &&
        case_index(n.result_type,Type::simple(TypeKind::Error))>=0;
    if(!fallible){
        symbols_.set_type(n.out,n.target_type);
        builder_.call(symbols_.value(n.out),ptr,array_cast_name(n.source_type,n.target_type),{{ptr,symbols_.value(n.array)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        symbols_.set_type(n.out,n.result_type);
        const auto result=symbols_.value(n.out);
        const auto ok=names_.value("array.cast.ok");
        const auto yes=names_.label("array.cast.value");
        const auto bad=names_.label("array.cast.error");
        const auto done=names_.label("array.cast.done");
        union_box_.allocate(result,n.result_type);
        builder_.call(ok,i1,array_cast_validate_name(n.source_type,n.target_type),{{ptr,symbols_.value(n.array)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        builder_.br(ok,yes,bad);
        builder_.block(yes);
        const auto converted=names_.value("array.cast.value");
        builder_.call(converted,ptr,array_cast_name(n.source_type,n.target_type),{{ptr,symbols_.value(n.array)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        union_box_.store_tag(result,case_index(n.result_type,n.target_type));
        const auto payload=names_.value("array.cast.payload");
        union_box_.payload_slot(payload,result);
        builder_.store({ptr,converted},payload,Align::none);
        builder_.br(done);
        builder_.block(bad);
        union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
        const auto error_payload=names_.value("array.cast.error.payload");
        union_box_.payload_slot(error_payload,result);
        // A fixed-width or bigint element fails only outside the
        // destination's range; an exact real element for the reason its
        // validator recorded.
        const auto& source_leaf=array_leaf(n.source_type);
        const auto reason=source_leaf.kind==TypeKind::Real
            ? recorded_failure_reason(error_payload)
            : reason_operand(abi::ConversionReason::out_of_range);
        store_conversion_message(error_payload,conversion_type(array_leaf(n.target_type)),
                                 subject_code(abi::ConversionSubject::array_element),reason);
        builder_.br(done);
        builder_.block(done);
    }
}

// The range checks of a numeric conversion, shared by both conversions:
// the i1 conditions under which the converted value would be outside the
// target type's range, which FallibleNumericConvert turns into its error
// case and a checked NumericConvert into a fail-fast guard.
void ConversionEmitter::add_integer_range_checks(std::vector<std::string>& checks,const Type& source,
                                                 const Type& target,const std::string& value){
    const auto source_ty=llvm_type(source);
    const auto source_width=integer_width(source), target_width=integer_width(target);
    if(is_signed_integer(source)&&!is_signed_integer(target)){
        const auto c=names_.value("cast.neg");
        builder_.icmp(c,IntPredicate::slt,source_ty,value,0);
        checks.push_back(c);
        if(target_width<source_width){
            const unsigned long long max=(target_width==64)?~0ULL:((1ULL<<target_width)-1ULL);
            const auto c2=names_.value("cast.high");
            builder_.icmp(c2,IntPredicate::sgt,source_ty,value,max);
            checks.push_back(c2);
        }
    }else if(!is_signed_integer(source)&&is_signed_integer(target)){
        if(target_width<=source_width){
            const unsigned long long max=(target_width==64)?0x7fffffffffffffffULL:((1ULL<<(target_width-1))-1ULL);
            const auto c=names_.value("cast.high");
            builder_.icmp(c,IntPredicate::ugt,source_ty,value,max);
            checks.push_back(c);
        }
    }else if(is_signed_integer(source)&&is_signed_integer(target)&&target_width<source_width){
        const long long min=-(1LL<<(target_width-1));
        const long long max=(1LL<<(target_width-1))-1;
        const auto lo=names_.value("cast.low"),hi=names_.value("cast.high");
        builder_.icmp(lo,IntPredicate::slt,source_ty,value,min);
        builder_.icmp(hi,IntPredicate::sgt,source_ty,value,max);
        checks.push_back(lo);checks.push_back(hi);
    }else if(!is_signed_integer(source)&&!is_signed_integer(target)&&target_width<source_width){
        const unsigned long long max=(target_width==64)?~0ULL:((1ULL<<target_width)-1ULL);
        const auto c=names_.value("cast.high");
        builder_.icmp(c,IntPredicate::ugt,source_ty,value,max);
        checks.push_back(c);
    }
}

std::string ConversionEmitter::any_check(const std::vector<std::string>& checks){
    std::string bad=checks.front();
    for(std::size_t i=1;i<checks.size();++i){
        const auto joined=names_.value("cast.bad");
        builder_.binary(joined,BinaryOp::or_,i1,bad,checks[i]);
        bad=joined;
    }
    return bad;
}

std::string ConversionEmitter::float32_range_check(const std::string& value){
    const auto finite=names_.value("cast.finite");
    const auto high=names_.value("cast.high");
    const auto low=names_.value("cast.low");
    const auto outside=names_.value("cast.outside");
    const auto bad=names_.value("cast.bad");
    builder_.fcmp(finite,FloatPredicate::ord,double_type,value,value);
    builder_.fcmp(high,FloatPredicate::ogt,double_type,value,"0x47EFFFFFE0000000");
    builder_.fcmp(low,FloatPredicate::olt,double_type,value,"0xC7EFFFFFE0000000");
    builder_.binary(outside,BinaryOp::or_,i1,high,low);
    builder_.binary(bad,BinaryOp::and_,i1,finite,outside);
    return bad;
}

void ConversionEmitter::emit(const ir::FallibleNumericConvert& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto source_ty=llvm_type(n.source_type), target_ty=llvm_type(n.target_type);
    const auto source_width=integer_width(n.source_type), target_width=integer_width(n.target_type);
    const auto result=symbols_.value(n.out);
    union_box_.allocate(result,n.result_type);
    std::vector<std::string> checks;
    std::string converted;
    // How the error block names the reason: a narrowed real64 tells
    // ±infinity from a finite value, an exact real reads the reason its
    // try-conversion recorded, every other conversion fails only outside
    // the destination's range.
    // A real converted to bigint declines a small non-integral rational
    // inline (NOT_INTEGRAL), so only a general value's reason is recorded.
    enum class ReasonSource { out_of_range, float32_narrowing, recorded, recorded_or_fraction };
    auto failure_reason=ReasonSource::out_of_range;
    if(n.source_type.kind==TypeKind::Real) failure_reason=ReasonSource::recorded;
    if(n.source_type.kind==TypeKind::Real&&is_bare_integer(n.target_type))
        failure_reason=ReasonSource::recorded_or_fraction;
    // A word the conversion owns once it succeeds: retained on success (a
    // copy of the source) or released on failure (a temporary).
    std::string retain_on_success, release_on_failure;
    if(n.target_type.kind==TypeKind::Nat&&n.source_type.kind==TypeKind::Int){
        const auto negative=names_.value("cast.nat.negative");
        builder_.call(negative,i1,LlvmOperand::global(int_helper::lt),{{bare_integer_type,symbols_.value(n.value)},{bare_integer_type,0}});
        checks.push_back(negative);
        converted=names_.value("cast.value");
        copy_by_add_zero(builder_,converted,bare_integer_type,symbols_.value(n.value));
        retain_on_success=converted;
    }else if(n.target_type.kind==TypeKind::Nat&&is_signed_integer(n.source_type)){
        std::string widened=symbols_.value(n.value);
        if(source_width<64){
            widened=names_.value("cast.nat.widen");
            builder_.cast(widened,CastOp::sext,{source_ty,symbols_.value(n.value)},i64);
        }
        const auto negative=names_.value("cast.nat.negative");
        builder_.icmp(negative,IntPredicate::slt,i64,widened,0);
        checks.push_back(negative);
        const auto nonnegative=names_.value("cast.nat.value");
        builder_.select(nonnegative,negative,{i64,0},{i64,widened});
        converted=names_.value("cast.value");
        builder_.call(converted,bare_integer_type,LlvmOperand::global(int_helper::from_i64),{{i64,nonnegative}});
    }else if(is_fixed_integer(n.source_type)&&is_fixed_integer(n.target_type)){
        add_integer_range_checks(checks,n.source_type,n.target_type,symbols_.value(n.value));
        converted=names_.value("cast.value");
        if(source_width==target_width)
            copy_by_add_zero(builder_,converted,target_ty,symbols_.value(n.value));
        else if(target_width<source_width)
            builder_.cast(converted,CastOp::trunc,{source_ty,symbols_.value(n.value)},target_ty);
        else
            builder_.cast(converted,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{source_ty,symbols_.value(n.value)},target_ty);
    }else if(n.source_type.kind==TypeKind::Real64&&n.target_type.kind==TypeKind::Real32){
        failure_reason=ReasonSource::float32_narrowing;
        checks.push_back(float32_range_check(symbols_.value(n.value)));
        converted=names_.value("cast.value");
        builder_.cast(converted,CastOp::fptrunc,{double_type,symbols_.value(n.value)},float_type);
    }else if(is_bare_integer(n.source_type)&&is_fixed_integer(n.target_type)){
        const auto& slot=scratch_.shared_slot(ScratchPlanner::SharedSlot::FallibleExactI64);
        if(slot.empty()) throw std::logic_error("missing planned exact integer cast scratch slot");
        const auto ok=names_.value("cast.exact.ok");
        builder_.call(ok,i1,LlvmOperand::global(is_signed_integer(n.target_type)?int_helper::try_i64:int_helper::try_u64),{{bare_integer_type,symbols_.value(n.value)},{i32,target_width},{ptr,slot}});
        const auto raw=names_.value("cast.exact.raw");
        builder_.load(raw,i64,slot,Align::eight);
        converted=names_.value("cast.value");
        if(target_width<64)
            builder_.cast(converted,CastOp::trunc,{i64,raw},target_ty);
        else
            copy_by_add_zero(builder_,converted,i64,raw);
        const auto bad=names_.value("cast.exact.bad");
        builder_.binary(bad,BinaryOp::xor_,i1,ok,"true");
        checks.push_back(bad);
    }else if(n.source_type.kind==TypeKind::Real&&
             is_bare_integer(n.target_type)){
        const auto& slot=scratch_.shared_slot(ScratchPlanner::SharedSlot::FallibleExactI64);
        if(slot.empty()) throw std::logic_error("missing planned exact integer cast scratch slot");
        const auto ok=names_.value("cast.exact.ok");
        builder_.call(ok,i1,LlvmOperand::global(real_helper::try_bigint),{{exact_real_type,symbols_.value(n.value)},{ptr,slot}});
        converted=names_.value("cast.value");
        builder_.load(converted,bare_integer_type,slot,Align::eight);
        const auto bad=names_.value("cast.exact.bad");
        builder_.binary(bad,BinaryOp::xor_,i1,ok,"true");
        checks.push_back(bad);
        if(n.target_type.kind==TypeKind::Nat){
            const auto negative=names_.value("cast.nat.negative");
            builder_.call(negative,i1,LlvmOperand::global(int_helper::lt),{{bare_integer_type,converted},{bare_integer_type,0}});
            checks.push_back(negative);
            release_on_failure=converted;
        }
    }else if(n.source_type.kind==TypeKind::Real&&is_fixed_integer(n.target_type)){
        const auto& slot=scratch_.shared_slot(ScratchPlanner::SharedSlot::FallibleExactI64);
        if(slot.empty()) throw std::logic_error("missing planned exact integer cast scratch slot");
        const auto ok=names_.value("cast.exact.ok");
        builder_.call(ok,i1,LlvmOperand::global(is_signed_integer(n.target_type)?real_helper::try_i64:real_helper::try_u64),{{exact_real_type,symbols_.value(n.value)},{i32,target_width},{ptr,slot}});
        const auto raw=names_.value("cast.exact.raw");
        builder_.load(raw,i64,slot,Align::eight);
        converted=names_.value("cast.value");
        if(target_width<64)
            builder_.cast(converted,CastOp::trunc,{i64,raw},target_ty);
        else
            copy_by_add_zero(builder_,converted,i64,raw);
        const auto bad=names_.value("cast.exact.bad");
        builder_.binary(bad,BinaryOp::xor_,i1,ok,"true");
        checks.push_back(bad);
    }else if((is_bare_integer(n.source_type) ||
              n.source_type.kind==TypeKind::Real) &&
             is_fixed_real(n.target_type)){
        const auto& slot=n.target_type.kind==TypeKind::Real32
            ?scratch_.shared_slot(ScratchPlanner::SharedSlot::FallibleExactFloat32)
            :scratch_.shared_slot(ScratchPlanner::SharedSlot::FallibleExactFloat64);
        if(slot.empty()) throw std::logic_error("missing planned exact float cast scratch slot");
        const auto ok=names_.value("cast.exact.float.ok");
        const bool to_float32=n.target_type.kind==TypeKind::Real32;
        if(is_bare_integer(n.source_type)){
            builder_.call(ok,i1,LlvmOperand::global(to_float32?int_helper::try_float32:int_helper::try_float64),{{bare_integer_type,symbols_.value(n.value)},{ptr,slot}});
        }else{
            builder_.call(ok,i1,LlvmOperand::global(to_float32?real_helper::try_float32:real_helper::try_float64),{{exact_real_type,symbols_.value(n.value)},{ptr,slot}});
        }
        converted=names_.value("cast.value");
        builder_.load(converted,target_ty,slot,n.target_type.kind==TypeKind::Real32?Align::four:Align::eight);
        const auto bad=names_.value("cast.exact.float.bad");
        builder_.binary(bad,BinaryOp::xor_,i1,ok,"true");
        checks.push_back(bad);
    }else{
        throw std::logic_error("unsupported fallible numeric conversion");
    }
    const std::string bad=checks.empty()?"false":any_check(checks);
    const auto fail_label=names_.label("cast.error");
    const auto ok_label=names_.label("cast.ok");
    const auto done_label=names_.label("cast.done");
    builder_.br(bad,fail_label,ok_label);
    builder_.block(fail_label);
    if(!release_on_failure.empty())
        builder_.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,release_on_failure}});
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto err_payload=names_.value("cast.error.payload");
    union_box_.payload_slot(err_payload,result);
    std::string reason=reason_operand(abi::ConversionReason::out_of_range);
    if(failure_reason==ReasonSource::float32_narrowing)
        reason=float32_failure_reason(symbols_.value(n.value),err_payload);
    else if(failure_reason==ReasonSource::recorded)
        reason=recorded_failure_reason(err_payload);
    else if(failure_reason==ReasonSource::recorded_or_fraction)
        reason=fraction_or_recorded_reason(symbols_.value(n.value),err_payload);
    store_conversion_message(err_payload,conversion_type(n.target_type),
                             subject_code(abi::ConversionSubject::value),reason);
    builder_.br(done_label);
    builder_.block(ok_label);
    if(!retain_on_success.empty())
        builder_.call(void_type,LlvmOperand::global(int_helper::retain),{{bare_integer_type,retain_on_success}});
    union_box_.store_tag(result,case_index(n.result_type,n.target_type));
    const auto ok_payload=names_.value("cast.payload");
    union_box_.payload_slot(ok_payload,result);
    builder_.store({target_ty,converted},ok_payload,Align::one);
    builder_.br(done_label);
    builder_.block(done_label);
}

void ConversionEmitter::emit(const ir::NumericConvert& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.target_type);
    const auto source_ty=llvm_type(n.source_type), target_ty=llvm_type(n.target_type);
    const auto source_width=integer_width(n.source_type), target_width=integer_width(n.target_type);
    // A checked conversion fails only outside the destination's range, except
    // a narrowed real64, whose failure block tells ±infinity apart.
    auto out_of_range=[](const std::string&){
        return reason_operand(abi::ConversionReason::out_of_range);
    };
    auto emit_fail_check=[&](const std::vector<std::string>& conditions,
                             const std::function<std::string(const std::string&)>& reason){
        if(conditions.empty())return;
        const auto bad=any_check(conditions);
        fail_fast_.fail_conversion_if(bad,conversion_type(n.target_type),
                                      subject_code(abi::ConversionSubject::value),reason,"cast",
                                      n.line,n.column);
    };
    if(n.source_type==n.target_type &&
       (is_fixed_integer(n.source_type)||is_fixed_real(n.source_type))){
        copy_by_select_true(builder_,symbols_.value(n.out),target_ty,symbols_.value(n.value));
    }else if(n.source_type.kind==TypeKind::Real&&n.source_type==n.target_type){
        builder_.call(void_type,LlvmOperand::global(real_helper::retain),{{exact_real_type,symbols_.value(n.value)}});
        copy_by_select_true(builder_,symbols_.value(n.out),exact_real_type,symbols_.value(n.value));
    }else if(n.source_type.kind==TypeKind::Int&&n.target_type.kind==TypeKind::Nat){
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_from_int),{{bare_integer_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(n.target_type.kind==TypeKind::Nat&&is_signed_integer(n.source_type)){
        std::string widened=symbols_.value(n.value);
        if(source_width<64){
            widened=names_.value("cast.nat.widen");
            builder_.cast(widened,CastOp::sext,{source_ty,symbols_.value(n.value)},i64);
        }
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_from_i64),{{i64,widened},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(n.source_type.kind==TypeKind::Real&&n.target_type.kind==TypeKind::Nat){
        const auto exact=names_.value("cast.nat.exact");
        builder_.call(exact,bare_integer_type,LlvmOperand::global(real_helper::to_bigint),{{exact_real_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_from_int),{{bare_integer_type,exact},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        builder_.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,exact}});
    }else if(is_bare_integer(n.source_type)&&is_bare_integer(n.target_type)){
        // The same word: nat to int, or the identity.
        builder_.call(void_type,LlvmOperand::global(int_helper::retain),{{bare_integer_type,symbols_.value(n.value)}});
        copy_by_add_zero(builder_,symbols_.value(n.out),bare_integer_type,symbols_.value(n.value));
    }else if(is_fixed_integer(n.source_type)&&is_bare_integer(n.target_type)&&n.inline_proven){
        // The value is proven inline: its word is the value shifted.
        std::string widened=symbols_.value(n.value);
        if(integer_width(n.source_type)<64){
            widened=names_.value("cast.bigint.widen");
            builder_.cast(widened,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{source_ty,symbols_.value(n.value)},i64);
        }
        builder_.binary(symbols_.value(n.out),BinaryOp::shl,NoWrap::nsw,bare_integer_type,widened,1);
    }else if(is_bare_integer(n.source_type)&&is_fixed_integer(n.target_type)&&n.inline_proven){
        // The word is proven inline and its value within the target's range.
        const auto raw=names_.value("cast.bigint.integer");
        builder_.binary(raw,BinaryOp::ashr,bare_integer_type,symbols_.value(n.value),1);
        if(target_width<64) builder_.cast(symbols_.value(n.out),CastOp::trunc,{i64,raw},target_ty);
        else copy_by_add_zero(builder_,symbols_.value(n.out),i64,raw);
    }else if(is_fixed_integer(n.source_type)&&is_bare_integer(n.target_type)){
        std::string widened=symbols_.value(n.value);
        if(integer_width(n.source_type)<64){
            widened=names_.value("cast.bigint.widen");
            builder_.cast(widened,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{source_ty,symbols_.value(n.value)},i64);
        }
        if(integer_width(n.source_type)<64){
            // Every value of a narrower integer type is inline.
            builder_.binary(symbols_.value(n.out),BinaryOp::shl,NoWrap::nsw,bare_integer_type,widened,1);
        }else{
            builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(is_signed_integer(n.source_type)?int_helper::from_i64:int_helper::from_u64),{{i64,widened}});
        }
    }else if(is_fixed_integer(n.source_type)&&n.target_type.kind==TypeKind::Real){
        std::string widened=symbols_.value(n.value);
        if(integer_width(n.source_type)<64){
            widened=names_.value("cast.bigreal.widen");
            builder_.cast(widened,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{source_ty,symbols_.value(n.value)},i64);
        }
        builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(is_signed_integer(n.source_type)?real_helper::from_i64:real_helper::from_u64),{{i64,widened}});
    }else if(is_bare_integer(n.source_type)&&n.target_type.kind==TypeKind::Real){
        builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::from_bigint),{{bare_integer_type,symbols_.value(n.value)}});
    }else if(is_fixed_real(n.source_type)&&n.target_type.kind==TypeKind::Real){
        std::string widened=symbols_.value(n.value);
        if(n.source_type.kind==TypeKind::Real32){
            widened=names_.value("cast.bigreal.float");
            builder_.cast(widened,CastOp::fpext,{float_type,symbols_.value(n.value)},double_type);
        }
        builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::from_float64),{{double_type,widened}});
    }else if(n.source_type.kind==TypeKind::Real&&is_bare_integer(n.target_type)){
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(real_helper::to_bigint),{{exact_real_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(is_bare_integer(n.source_type)&&is_fixed_integer(n.target_type)){
        const auto raw=names_.value("cast.bigint.integer");
        builder_.call(raw,bare_integer_type,LlvmOperand::global(is_signed_integer(n.target_type)?int_helper::to_i64:int_helper::to_u64),{{bare_integer_type,symbols_.value(n.value)},{i32,target_width},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        if(target_width<64) builder_.cast(symbols_.value(n.out),CastOp::trunc,{i64,raw},target_ty);
        else copy_by_add_zero(builder_,symbols_.value(n.out),i64,raw);
    }else if(n.source_type.kind==TypeKind::Real&&is_fixed_integer(n.target_type)){
        const auto exact=names_.value("cast.bigreal.integer.exact");
        const auto raw=names_.value("cast.bigreal.integer");
        builder_.call(exact,bare_integer_type,LlvmOperand::global(real_helper::to_bigint),{{exact_real_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        builder_.call(raw,bare_integer_type,LlvmOperand::global(is_signed_integer(n.target_type)?int_helper::to_i64:int_helper::to_u64),{{bare_integer_type,exact},{i32,target_width},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        builder_.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,exact}});
        if(target_width<64) builder_.cast(symbols_.value(n.out),CastOp::trunc,{i64,raw},target_ty);
        else copy_by_add_zero(builder_,symbols_.value(n.out),i64,raw);
    }else if(is_bare_integer(n.source_type)&&is_fixed_real(n.target_type)){
        if(n.target_type.kind==TypeKind::Real32)
            builder_.call(symbols_.value(n.out),float_type,LlvmOperand::global(int_helper::to_float32),{{bare_integer_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        else
            builder_.call(symbols_.value(n.out),double_type,LlvmOperand::global(int_helper::to_float64),{{bare_integer_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(n.source_type.kind==TypeKind::Real&&is_fixed_real(n.target_type)){
        if(n.target_type.kind==TypeKind::Real32)
            builder_.call(symbols_.value(n.out),float_type,LlvmOperand::global(real_helper::to_float32),{{exact_real_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        else
            builder_.call(symbols_.value(n.out),double_type,LlvmOperand::global(real_helper::to_float64),{{exact_real_type,symbols_.value(n.value)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(is_fixed_integer(n.source_type)&&is_fixed_integer(n.target_type)){
        std::vector<std::string> checks;
        if(n.checked_range) add_integer_range_checks(checks,n.source_type,n.target_type,symbols_.value(n.value));
        emit_fail_check(checks,out_of_range);
        if(source_width==target_width) copy_by_add_zero(builder_,symbols_.value(n.out),target_ty,symbols_.value(n.value));
        else if(target_width<source_width) builder_.cast(symbols_.value(n.out),CastOp::trunc,{source_ty,symbols_.value(n.value)},target_ty);
        else builder_.cast(symbols_.value(n.out),is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{source_ty,symbols_.value(n.value)},target_ty);
    }else if(is_fixed_integer(n.source_type)&&is_fixed_real(n.target_type)){
        builder_.cast(symbols_.value(n.out),is_signed_integer(n.source_type)?CastOp::sitofp:CastOp::uitofp,{source_ty,symbols_.value(n.value)},target_ty);
        if(n.checked_range){
            const auto back=names_.value("cast.back");
            const auto sat=runtime_abi::llvm_intrinsic::saturating_float_to_integer(
                is_signed_integer(n.source_type),source_width,n.target_type.kind==TypeKind::Real32);
            builder_.call(back,source_ty,sat,{{target_ty,symbols_.value(n.out)}});
            const auto mismatch=names_.value("cast.mismatch");
            builder_.icmp(mismatch,IntPredicate::ne,source_ty,back,symbols_.value(n.value));
            emit_fail_check({mismatch},out_of_range);
        }
    }else if(is_fixed_real(n.source_type)&&is_fixed_integer(n.target_type)){
        const auto intrinsic=runtime_abi::llvm_intrinsic::saturating_float_to_integer(
            is_signed_integer(n.target_type),target_width,n.source_type.kind==TypeKind::Real32);
        builder_.call(symbols_.value(n.out),target_ty,intrinsic,{{source_ty,symbols_.value(n.value)}});
        if(n.checked_range){
            const auto back=names_.value("cast.back"),same=names_.value("cast.same"),bad=names_.value("cast.bad");
            builder_.cast(back,is_signed_integer(n.target_type)?CastOp::sitofp:CastOp::uitofp,{target_ty,symbols_.value(n.out)},source_ty);
            builder_.fcmp(same,FloatPredicate::oeq,source_ty,back,symbols_.value(n.value));
            builder_.binary(bad,BinaryOp::xor_,i1,same,"true");
            emit_fail_check({bad},out_of_range);
        }
    }else if(is_fixed_real(n.source_type)&&is_fixed_real(n.target_type)){
        if(n.source_type.kind==TypeKind::Real32&&n.target_type.kind==TypeKind::Real64){
            builder_.cast(symbols_.value(n.out),CastOp::fpext,{float_type,symbols_.value(n.value)},double_type);
        }else if(n.source_type.kind==TypeKind::Real64&&n.target_type.kind==TypeKind::Real32){
            if(n.checked_range){
                const auto bad=float32_range_check(symbols_.value(n.value));
                const auto value=symbols_.value(n.value);
                emit_fail_check({bad},[&](const std::string& label){
                    return float32_failure_reason(value,"%"+label);
                });
            }
            builder_.cast(symbols_.value(n.out),CastOp::fptrunc,{double_type,symbols_.value(n.value)},float_type);
        }else{
            copy_by_fadd_zero(builder_,symbols_.value(n.out),target_ty,symbols_.value(n.value));
        }
    }
}

void ConversionEmitter::emit(const ir::ParseBin& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto parsed=names_.value("bin.parse.value");
    builder_.call(parsed,runtime_abi::bin::parse,{{ptr,symbols_.value(n.text)}});
    if(n.result_type.kind==TypeKind::Bin){
        copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::no,parsed);
    }else{
        const auto result=symbols_.value(n.out);
        union_box_.allocate(result,n.result_type);
        if(n.success_proven){
            union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Bin)));
            const auto ok_payload=names_.value("bin.parse.payload");
            union_box_.payload_slot(ok_payload,result);
            builder_.store({ptr,parsed},ok_payload,Align::none);
        }else{
            const auto ok=names_.value("bin.parse.ok");
            const auto ok_label=names_.label("bin.parse.ok");
            const auto fail_label=names_.label("bin.parse.fail");
            const auto done_label=names_.label("bin.parse.done");
            builder_.icmp(ok,IntPredicate::ne,ptr,parsed,"null");
            builder_.br(ok,ok_label,fail_label);
            builder_.block(ok_label);
            union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Bin)));
            const auto ok_payload=names_.value("bin.parse.payload");
            union_box_.payload_slot(ok_payload,result);
            builder_.store({ptr,parsed},ok_payload,Align::none);
            builder_.br(done_label);
            builder_.block(fail_label);
            union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
            const auto err_payload=names_.value("bin.parse.error.payload");
            union_box_.payload_slot(err_payload,result);
            builder_.store({ptr,"@.err.parse"},err_payload,Align::none);
            builder_.br(done_label);
            builder_.block(done_label);
        }
    }
}

void ConversionEmitter::emit(const ir::BinConvert& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.target_type);
    if(n.source_type.kind==TypeKind::Bin){
        if(n.target_type.kind==TypeKind::Array){
            const auto width=n.target_type.first->kind==TypeKind::Bool?1:integer_width(*n.target_type.first);
            const auto stride=runtime_storage_bytes(*n.target_type.first);
            builder_.call(symbols_.value(n.out),runtime_abi::bin::to_array,{{ptr,symbols_.value(n.value)},{i32,width},{i32,stride}});
        }else{
            const auto width=n.target_type.kind==TypeKind::Bool?1:integer_width(n.target_type);
            const auto raw=names_.value("bin.scalar");
            builder_.call(raw,runtime_abi::bin::to_u64,{{ptr,symbols_.value(n.value)},{i32,width}});
            if(n.target_type.kind==TypeKind::Bool){
                builder_.cast(symbols_.value(n.out),CastOp::trunc,{i64,raw},i1);
            }else if(width<64){
                builder_.cast(symbols_.value(n.out),CastOp::trunc,{i64,raw},llvm_type(n.target_type));
            }else{
                copy_by_add_zero(builder_,symbols_.value(n.out),i64,raw);
            }
        }
    }else if(n.target_type.kind==TypeKind::Bin){
        if(n.source_type.kind==TypeKind::Array){
            const auto width=n.source_type.first->kind==TypeKind::Bool?1:integer_width(*n.source_type.first);
            const auto stride=runtime_storage_bytes(*n.source_type.first);
            builder_.call(symbols_.value(n.out),runtime_abi::bin::from_array,{{ptr,symbols_.value(n.value)},{i32,width},{i32,stride}});
        }else{
            const auto width=n.source_type.kind==TypeKind::Bool?1:integer_width(n.source_type);
            std::string widened=symbols_.value(n.value);
            if(n.source_type.kind==TypeKind::Bool){
                widened=names_.value("bin.bool");
                builder_.cast(widened,CastOp::zext,{i1,symbols_.value(n.value)},i64);
            }else if(width<64){
                widened=names_.value("bin.integer");
                builder_.cast(widened,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{llvm_type(n.source_type),symbols_.value(n.value)},i64);
            }
            builder_.call(symbols_.value(n.out),runtime_abi::bin::from_u64,{{i64,widened},{i32,width}});
        }
    }
}

void ConversionEmitter::emit(const ir::ParseNumber& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,n.result_type);
    const auto result=symbols_.value(n.out);
    if(n.target_type.kind==TypeKind::Real){
        const auto parsed=names_.value("parse.exact"),ok=names_.value("parse.exact.ok");
        builder_.call(parsed,exact_real_type,LlvmOperand::global(real_helper::parse),{{ptr,symbols_.value(n.text)}});
        if(n.result_type==n.target_type){
            copy_by_select_true(builder_,symbols_.value(n.out),exact_real_type,parsed);
            return;
        }
        union_box_.allocate(result,n.result_type);
        const auto none=names_.value("parse.exact.none");
        builder_.call(none,i1,LlvmOperand::global(real_helper::is_none),{{exact_real_type,parsed}});
        builder_.binary(ok,BinaryOp::xor_,i1,none,"true");
        const auto yes=names_.label("parse.exact.ok"),bad=names_.label("parse.exact.error"),done=names_.label("parse.exact.done");
        builder_.br(ok,yes,bad);
        builder_.block(yes);
        union_box_.store_tag(result,case_index(n.result_type,n.target_type));
        const auto payload=names_.value("parse.exact.payload");
        union_box_.payload_slot(payload,result);
        builder_.store({exact_real_type,parsed},payload,Align::none);
        builder_.br(done);
        builder_.block(bad);
        union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
        const auto ep=names_.value("parse.exact.error.payload");
        union_box_.payload_slot(ep,result);
        builder_.store({ptr,"@.err.parse"},ep,Align::none);
        builder_.br(done);
        builder_.block(done);
        return;
    }
    if(is_bare_integer(n.target_type)){
        const auto parsed=names_.value("parse.exact"),ok=names_.value("parse.exact.ok");
        const auto& slot=scratch_.instruction_slot(ins);
        builder_.call(ok,i1,LlvmOperand::global(int_helper::parse),{{ptr,symbols_.value(n.text)},{ptr,slot}});
        builder_.load(parsed,bare_integer_type,slot,Align::none);
        if(n.result_type==n.target_type){
            copy_by_add_zero(builder_,symbols_.value(n.out),bare_integer_type,parsed);
            return;
        }
        union_box_.allocate(result,n.result_type);
        const auto yes=names_.label("parse.exact.ok"),bad=names_.label("parse.exact.error"),done=names_.label("parse.exact.done");
        builder_.br(ok,yes,bad);
        builder_.block(yes);
        union_box_.store_tag(result,case_index(n.result_type,n.target_type));
        const auto payload=names_.value("parse.exact.payload");
        union_box_.payload_slot(payload,result);
        builder_.store({bare_integer_type,parsed},payload,Align::none);
        builder_.br(done);
        builder_.block(bad);
        union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
        const auto ep=names_.value("parse.exact.error.payload");
        union_box_.payload_slot(ep,result);
        builder_.store({ptr,"@.err.parse"},ep,Align::none);
        builder_.br(done);
        builder_.block(done);
        return;
    }
    union_box_.allocate(result,n.result_type);

    std::string parsed;
    std::string parse_ok;
    if(is_fixed_integer(n.target_type)){
        const auto& slot=scratch_.instruction_slot(ins);
        parsed=names_.value("parse.integer");
        parse_ok=names_.value("parse.ok");
        builder_.call(parse_ok,is_signed_integer(n.target_type)?runtime_abi::numeric::parse_signed:runtime_abi::numeric::parse_unsigned,{{ptr,symbols_.value(n.text)},{ptr,slot}});
        builder_.load(parsed,i64,slot,Align::none);
    }else{
        const auto& slot=scratch_.instruction_slot(ins);
        parse_ok=names_.value("parse.ok");
        if(n.target_type.kind==TypeKind::Real32){
            parsed=names_.value("parse.float");
            builder_.call(parse_ok,runtime_abi::numeric::parse_float32,{{ptr,symbols_.value(n.text)},{ptr,slot}});
            builder_.load(parsed,float_type,slot,Align::none);
        }else{
            parsed=names_.value("parse.float");
            builder_.call(parse_ok,runtime_abi::numeric::parse_float64,{{ptr,symbols_.value(n.text)},{ptr,slot}});
            builder_.load(parsed,double_type,slot,Align::none);
        }
    }

    std::string bad=names_.value("parse.bad");
    builder_.binary(bad,BinaryOp::xor_,i1,parse_ok,"true");
    if(is_fixed_integer(n.target_type)){
        const auto width=integer_width(n.target_type);
        if(width<64){
            if(is_signed_integer(n.target_type)){
                const long long min_value=-(1LL<<(width-1));
                const long long max_value=(1LL<<(width-1))-1;
                const auto low=names_.value("parse.low"),high=names_.value("parse.high"),outside=names_.value("parse.outside"),combined=names_.value("parse.bad.width");
                builder_.icmp(low,IntPredicate::slt,i64,parsed,min_value);
                builder_.icmp(high,IntPredicate::sgt,i64,parsed,max_value);
                builder_.binary(outside,BinaryOp::or_,i1,low,high);
                builder_.binary(combined,BinaryOp::or_,i1,bad,outside);
                bad=combined;
            }else{
                const unsigned long long max_value=(1ULL<<width)-1ULL;
                const auto high=names_.value("parse.high"),combined=names_.value("parse.bad.width");
                builder_.icmp(high,IntPredicate::ugt,i64,parsed,max_value);
                builder_.binary(combined,BinaryOp::or_,i1,bad,high);
                bad=combined;
            }
        }
    }

    const auto fail_label=names_.label("parse.fail"),ok_label=names_.label("parse.ok"),done_label=names_.label("parse.done");
    builder_.br(bad,fail_label,ok_label);
    const auto error_tag=case_index(n.result_type,Type::simple(TypeKind::Error));
    const auto target_tag=case_index(n.result_type,n.target_type);
    builder_.block(fail_label);
    union_box_.store_tag(result,error_tag);
    const auto fail_payload=names_.value("parse.fail.payload");
    union_box_.payload_slot(fail_payload,result);
    builder_.store({ptr,"@.err.parse"},fail_payload,Align::none);
    builder_.br(done_label);
    builder_.block(ok_label);
    union_box_.store_tag(result,target_tag);
    const auto ok_payload=names_.value("parse.ok.payload");
    union_box_.payload_slot(ok_payload,result);
    if(is_fixed_integer(n.target_type)){
        const auto width=integer_width(n.target_type);
        if(width<64){
            const auto narrowed=names_.value("parse.narrow");
            builder_.cast(narrowed,CastOp::trunc,{i64,parsed},llvm_type(n.target_type));
            builder_.store({llvm_type(n.target_type),narrowed},ok_payload,Align::none);
        }else{
            builder_.store({i64,parsed},ok_payload,Align::none);
        }
    }else{
        builder_.store({llvm_type(n.target_type),parsed},ok_payload,Align::none);
    }
    builder_.br(done_label);
    builder_.block(done_label);
}

void ConversionEmitter::emit(const ir::ParseNumberDirect& n,const ir::Instruction& ins){
    symbols_.set_type(n.value_out,n.target_type);
    symbols_.set_type(n.ok_out,Type::simple(TypeKind::Bool));
    symbols_.set_type(n.error_out,Type::simple(TypeKind::Error));
    const auto& slot=scratch_.instruction_slot(ins);
    std::string parse_ok;
    std::string parsed;
    if(is_fixed_integer(n.target_type)){
        builder_.store({i64,0},slot,Align::none);
        parse_ok=names_.value("parse.direct.ok");
        parsed=names_.value("parse.direct.integer");
        builder_.call(parse_ok,is_signed_integer(n.target_type)?runtime_abi::numeric::parse_signed:runtime_abi::numeric::parse_unsigned,{{ptr,symbols_.value(n.text)},{ptr,slot}});
        builder_.load(parsed,i64,slot,Align::none);
        std::string valid=parse_ok;
        const auto width=integer_width(n.target_type);
        if(width<64){
            if(is_signed_integer(n.target_type)){
                const long long min_value=-(1LL<<(width-1));
                const long long max_value=(1LL<<(width-1))-1;
                const auto low=names_.value("parse.direct.low");
                const auto high=names_.value("parse.direct.high");
                const auto inside=names_.value("parse.direct.inside");
                const auto range_ok=names_.value("parse.direct.range.ok");
                builder_.icmp(low,IntPredicate::sge,i64,parsed,min_value);
                builder_.icmp(high,IntPredicate::sle,i64,parsed,max_value);
                builder_.binary(inside,BinaryOp::and_,i1,low,high);
                builder_.binary(range_ok,BinaryOp::and_,i1,parse_ok,inside);
                valid=range_ok;
            }else{
                const unsigned long long max_value=(1ULL<<width)-1ULL;
                const auto inside=names_.value("parse.direct.inside");
                const auto range_ok=names_.value("parse.direct.range.ok");
                builder_.icmp(inside,IntPredicate::ule,i64,parsed,max_value);
                builder_.binary(range_ok,BinaryOp::and_,i1,parse_ok,inside);
                valid=range_ok;
            }
            builder_.cast(symbols_.value(n.value_out),CastOp::trunc,{i64,parsed},llvm_type(n.target_type));
        }else{
            copy_by_add_zero(builder_,symbols_.value(n.value_out),i64,parsed);
        }
        copy_by_xor_false(builder_,symbols_.value(n.ok_out),valid);
    }else if(n.target_type.kind==TypeKind::Real32){
        builder_.store({float_type,float_zero},slot,Align::none);
        parse_ok=names_.value("parse.direct.ok");
        builder_.call(parse_ok,runtime_abi::numeric::parse_float32,{{ptr,symbols_.value(n.text)},{ptr,slot}});
        builder_.load(symbols_.value(n.value_out),float_type,slot,Align::none);
        copy_by_xor_false(builder_,symbols_.value(n.ok_out),parse_ok);
    }else{
        builder_.store({double_type,float_zero},slot,Align::none);
        parse_ok=names_.value("parse.direct.ok");
        builder_.call(parse_ok,runtime_abi::numeric::parse_float64,{{ptr,symbols_.value(n.text)},{ptr,slot}});
        builder_.load(symbols_.value(n.value_out),double_type,slot,Align::none);
        copy_by_xor_false(builder_,symbols_.value(n.ok_out),parse_ok);
    }
    builder_.getelementptr(symbols_.value(n.error_out),Inbounds::yes,LlvmType::array(21,i8),"@.err.parse",{{i64,0},{i64,0}});
}

void ConversionEmitter::emit(const ir::ToString& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    if(is_bare_integer(n.source_type)){
        builder_.call(symbols_.value(n.out),ptr,LlvmOperand::global(int_helper::text),{{bare_integer_type,symbols_.value(n.value)}});
    }else if(n.source_type.kind==TypeKind::Real){
        builder_.call(symbols_.value(n.out),ptr,LlvmOperand::global(real_helper::text),{{exact_real_type,symbols_.value(n.value)},{i32,abi::exact_real_display_digits}});
    }else if(is_fixed_integer(n.source_type)){
        std::string widened=symbols_.value(n.value);
        if(integer_width(n.source_type)<64){
            widened=names_.value("text.int");
            builder_.cast(widened,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{llvm_type(n.source_type),symbols_.value(n.value)},i64);
        }
        builder_.call(symbols_.value(n.out),is_signed_integer(n.source_type)?runtime_abi::numeric::integer_text_signed:runtime_abi::numeric::integer_text_unsigned,{{i64,widened}});
    }else if(is_fixed_real(n.source_type)){
        std::string widened=symbols_.value(n.value);
        if(n.source_type.kind==TypeKind::Real32){
            widened=names_.value("text.float");
            builder_.cast(widened,CastOp::fpext,{float_type,symbols_.value(n.value)},double_type);
        }
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::float_text,{{double_type,widened}});
    }else if(n.source_type.kind==TypeKind::Bool){
        builder_.select(symbols_.value(n.out),symbols_.value(n.value),{ptr,"@.bool.true"},{ptr,"@.bool.false"});
    }else if(n.source_type.kind==TypeKind::Bin){
        builder_.call(symbols_.value(n.out),runtime_abi::bin::string,{{ptr,symbols_.value(n.value)}});
    }else if(n.source_type.kind==TypeKind::String||n.source_type.kind==TypeKind::Error){
        copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::no,symbols_.value(n.value));
    }
}

void ConversionEmitter::emit(const ir::FormatNumber& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    const auto integer=n.integer_width?std::to_string(*n.integer_width):"-1";
    const auto fractional=n.fractional_digits?std::to_string(*n.fractional_digits):"-1";
    const auto significant=n.significant_digits?std::to_string(*n.significant_digits):"-1";
    if(is_bare_integer(n.source_type)){
        builder_.call(symbols_.value(n.out),ptr,LlvmOperand::global(int_helper::format),{{bare_integer_type,symbols_.value(n.value)},{i32,integer},{i32,fractional},{i32,significant},{i32,n.zero?1:0}});
    }else if(n.source_type.kind==TypeKind::Real){
        const int precision=n.significant_digits?static_cast<int>(*n.significant_digits):abi::exact_real_display_digits;
        builder_.call(symbols_.value(n.out),ptr,LlvmOperand::global(real_helper::text),{{exact_real_type,symbols_.value(n.value)},{i32,precision}});
    }else if(is_fixed_integer(n.source_type)){
        std::string widened=symbols_.value(n.value);
        if(integer_width(n.source_type)<64){
            widened=names_.value("format.int");
            builder_.cast(widened,is_signed_integer(n.source_type)?CastOp::sext:CastOp::zext,{llvm_type(n.source_type),symbols_.value(n.value)},i64);
        }
        builder_.call(symbols_.value(n.out),is_signed_integer(n.source_type)?runtime_abi::numeric::format_signed:runtime_abi::numeric::format_unsigned,{{i64,widened},{i32,integer},{i32,fractional},{i32,significant},{i32,n.zero?1:0}});
    }else{
        std::string widened=symbols_.value(n.value);
        if(n.source_type.kind==TypeKind::Real32){
            widened=names_.value("format.float");
            builder_.cast(widened,CastOp::fpext,{float_type,symbols_.value(n.value)},double_type);
        }
        builder_.call(symbols_.value(n.out),runtime_abi::numeric::format_number,{{double_type,widened},{i32,integer},{i32,fractional},{i32,significant},{i32,n.zero?1:0}});
    }
}

} // namespace quidra::llvm_backend
