#include "llvm_backend/numeric_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/half_precision.hpp"
#include "llvm_backend/real_power.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "operator_policy.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/ir/dtype.hpp"

#include <array>
#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void NumericEmitter::reserve(const ir::ExactAtom& n,const ir::Instruction&){
    pool_.intern(n.provider);
}

void NumericEmitter::reserve(const ir::ExactUnary& n,const ir::Instruction&){
    pool_.intern(n.provider);
}

void NumericEmitter::reserve(const ir::ExactOp& n,const ir::Instruction&){
    pool_.intern(n.provider);
}

void NumericEmitter::reserve(const ir::ExactRound& n,const ir::Instruction&){
    if(!n.slot.empty())pool_.intern_exact_slot(n.slot,n.type.kind==TypeKind::Real32);
}

void NumericEmitter::reserve(const ir::ExactConstant& n,const ir::Instruction&){
    pool_.intern_exact_slot(n.slot,n.type.kind==TypeKind::Real32);
}

// The binary64 parts of a fixed-width complex power.
void NumericEmitter::reserve(const ir::Binary& n,const ir::Instruction& ins){
    if(n.op=="^"&&is_fixed_complex(n.operand_type))
        scratch_.reserve(ins,LlvmType::array(2,double_type),Align::eight);
}


namespace {
std::string loop_widen(LlvmBuilder& builder,TemporaryNames& names,const Type& type,
                      const std::string& value) {
    if(type.kind==TypeKind::Real64) return value;
    auto widened=names.value("real.loop.widen");
    if(type.kind==TypeKind::Real16b) {
        auto single=names.value("real.loop.bf16");
        builder.call(single,float_type,LlvmOperand::global(half_helper::bf16_widen),{{i16,value}});
        builder.cast(widened,CastOp::fpext,{float_type,single},double_type);
    } else builder.cast(widened,CastOp::fpext,{llvm_type(type),value},double_type);
    return widened;
}
}
void NumericEmitter::emit(const ir::RealLoopCount& n,const ir::Instruction&) {
    symbols_.set_type(n.out,Type::simple(TypeKind::Int));
    auto start=loop_widen(builder_,names_,n.type,symbols_.value(n.start));
    auto end=loop_widen(builder_,names_,n.type,symbols_.value(n.end));
    auto step=loop_widen(builder_,names_,n.type,symbols_.value(n.step));
    builder_.call(symbols_.value(n.out),runtime_abi::real_loop::count,
        {{double_type,start},{double_type,end},{double_type,step},{i1,n.descending?1:0},
         {i1,n.start_inclusive?1:0},{i1,n.end_inclusive?1:0},
         {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
}
void NumericEmitter::emit(const ir::RealLoopValue& n,const ir::Instruction&) {
    symbols_.set_type(n.out,n.type);
    const auto target=llvm_type(n.type);
    const bool has_native=n.type.kind==TypeKind::Real16||n.type.kind==TypeKind::Real32||n.type.kind==TypeKind::Real64;
    const auto fast=names_.label("real.loop.fma"),slow=names_.label("real.loop.exact"),done=names_.label("real.loop.ready");
    if(has_native) builder_.br(n.native_fma?symbols_.value(n.native_fma):"false",fast,slow);
    else builder_.br(slow);
    std::string fast_value;
    if(has_native) {
        builder_.block(fast);
        auto index=names_.value("real.loop.index"),floating=names_.value("real.loop.multiplier");
        builder_.binary(index,BinaryOp::ashr,i64,symbols_.value(n.multiplier),1);
        builder_.cast(floating,CastOp::sitofp,{i64,index},target);
        auto rounded=names_.value("real.loop.fused");
        const auto intrinsic=n.type.kind==TypeKind::Real16?"llvm.fma.f16":n.type.kind==TypeKind::Real32?"llvm.fma.f32":"llvm.fma.f64";
        builder_.call(rounded,target,LlvmOperand::global(intrinsic),
            {{target,floating},{target,symbols_.value(n.step)},{target,symbols_.value(n.start)}});
        auto first=names_.value("real.loop.first"),zero=names_.value("real.loop.zero"),preserve=names_.value("real.loop.preserve_zero");
        builder_.icmp(first,IntPredicate::eq,i64,index,0);
        builder_.fcmp(zero,FloatPredicate::oeq,target,symbols_.value(n.start),"0.0");
        builder_.binary(preserve,BinaryOp::and_,i1,first,zero);
        fast_value=names_.value("real.loop.native_value");
        builder_.select(fast_value,preserve,{target,symbols_.value(n.start)},{target,rounded});
        builder_.br(done);
    }
    builder_.block(slow);
    auto start=loop_widen(builder_,names_,n.type,symbols_.value(n.start));
    auto step=loop_widen(builder_,names_,n.type,symbols_.value(n.step));
    auto wide=names_.value("real.loop.rounded");
    const int format=n.type.kind==TypeKind::Real16?0:n.type.kind==TypeKind::Real16b?1:
                     n.type.kind==TypeKind::Real32?2:3;
    builder_.call(wide,runtime_abi::real_loop::value,
        {{double_type,start},{double_type,step},{i64,symbols_.value(n.multiplier)},{i32,format},{i1,"false"}});
    auto exact=wide;
    if(n.type.kind==TypeKind::Real16b) {
        auto bits=names_.value("real.loop.bf16.bits");exact=names_.value("real.loop.bf16.value");
        builder_.call(bits,runtime_abi::half_support::real16b_from_float64,{{double_type,wide}});
        builder_.cast(exact,CastOp::trunc,{i32,bits},i16);
    } else if(n.type.kind!=TypeKind::Real64) {
        exact=names_.value("real.loop.narrow");
        builder_.cast(exact,CastOp::fptrunc,{double_type,wide},target);
    }
    builder_.br(done);builder_.block(done);
    if(has_native) builder_.phi(symbols_.value(n.out),target,{{fast_value,fast},{exact,slow}});
    else builder_.phi(symbols_.value(n.out),target,{{exact,slow}});
}



void NumericEmitter::reserve(const ir::ExactFromScaled&,const ir::Instruction& ins) {
    scratch_.reserve(ins,exact_real_type,Align::eight);
}
void NumericEmitter::reserve(const ir::ExactLoopValue&,const ir::Instruction& ins) {
    scratch_.reserve(ins,exact_real_type,Align::eight);
}
namespace {
std::array<std::string,2> loop_real_parts(LlvmBuilder& builder,TemporaryNames& names,
                                        const std::string& value) {
    std::array<std::string,2> parts{names.value("loop.real.n"),names.value("loop.real.d")};
    builder.extractvalue(parts[0],exact_real_type,value,0);
    builder.extractvalue(parts[1],exact_real_type,value,1);
    return parts;
}
}
void NumericEmitter::emit(const ir::ExactLoopPlan& n,const ir::Instruction&) {
    symbols_.set_type(n.out,Type::array(Type::simple(TypeKind::Int)));
    auto start=loop_real_parts(builder_,names_,symbols_.value(n.start));
    auto end=loop_real_parts(builder_,names_,symbols_.value(n.end));
    auto step=loop_real_parts(builder_,names_,symbols_.value(n.step));
    builder_.call(symbols_.value(n.out),runtime_abi::real_loop::plan,
        {{i64,start[0]},{i64,start[1]},{i64,end[0]},{i64,end[1]},{i64,step[0]},{i64,step[1]},
         {i1,n.descending?1:0},{i1,n.start_inclusive?1:0},{i1,n.end_inclusive?1:0},
         {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
}
void NumericEmitter::emit(const ir::ExactFromScaled& n,const ir::Instruction& ins) {
    symbols_.set_type(n.out,Type::simple(TypeKind::Real));
    const auto& slot=scratch_.instruction_slot(ins);
    builder_.call(runtime_abi::real_loop::from_scaled,
        {{i64,symbols_.value(n.numerator)},{i64,symbols_.value(n.denominator)},{ptr,slot}});
    builder_.load(symbols_.value(n.out),exact_real_type,slot,Align::eight);
}
void NumericEmitter::emit(const ir::ExactLoopValue& n,const ir::Instruction& ins) {
    symbols_.set_type(n.out,Type::simple(TypeKind::Real));
    auto start=loop_real_parts(builder_,names_,symbols_.value(n.start));
    auto step=loop_real_parts(builder_,names_,symbols_.value(n.step));
    const auto& slot=scratch_.instruction_slot(ins);
    builder_.call(runtime_abi::real_loop::exact_value,
        {{i64,start[0]},{i64,start[1]},{i64,step[0]},{i64,step[1]},
         {i64,symbols_.value(n.multiplier)},{ptr,slot}});
    builder_.load(symbols_.value(n.out),exact_real_type,slot,Align::eight);
}
void NumericEmitter::emit(const ir::ExactScaledCompare& n,const ir::Instruction&) {
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    auto ordering=names_.value("exact.scaled.order");
    builder_.call(ordering,runtime_abi::real_loop::scaled_compare,
        {{i64,symbols_.value(n.numerator)},{i64,symbols_.value(n.denominator)},
         {i64,n.constant_n},{i64,n.constant_d}});
    const auto op=n.op=="<"?IntPredicate::slt:n.op=="<="?IntPredicate::sle:
                  n.op==">"?IntPredicate::sgt:n.op==">="?IntPredicate::sge:
                  n.op=="=="?IntPredicate::eq:IntPredicate::ne;
    builder_.icmp(symbols_.value(n.out),op,i32,ordering,0);
}
void NumericEmitter::reserve(const ir::ExactScaledConvert&,const ir::Instruction& ins) {
    scratch_.reserve(ins,double_type,Align::eight);
}
void NumericEmitter::emit(const ir::ExactScaledConvert& n,const ir::Instruction& ins) {
    const auto result_type=n.result_type.kind==TypeKind::Invalid?n.target:n.result_type;
    const bool fallible=result_type.kind==TypeKind::Union;
    symbols_.set_type(n.out,result_type);
    const auto& slot=scratch_.instruction_slot(ins);
    const int format=n.target.kind==TypeKind::Real16?0:n.target.kind==TypeKind::Real16b?1:
                     n.target.kind==TypeKind::Real32?2:3;
    const auto ok=names_.value("exact.scaled.ok");
    builder_.call(ok,runtime_abi::real_loop::scaled_convert,
        {{i64,symbols_.value(n.numerator)},{i64,symbols_.value(n.denominator)},{i32,format},{ptr,slot}});
    const auto success=names_.label("exact.scaled.value"),failed=names_.label("exact.scaled.error"),done=names_.label("exact.scaled.done");
    UnionBox box(builder_);
    if(fallible) box.allocate(symbols_.value(n.out),result_type);
    builder_.br(ok,success,failed);builder_.block(failed);
    if(fallible) {
        box.store_tag(symbols_.value(n.out),case_index(result_type,Type::simple(TypeKind::Error)));
        const auto payload=names_.value("exact.scaled.error_payload");
        box.payload_slot(payload,symbols_.value(n.out));
        records_.store_conversion(payload,abi::dtype_code(ir::dtype_of(n.target)),abi::ConversionSubject::value,
            std::to_string(static_cast<int>(abi::ConversionReason::out_of_range)),1,n.position.line,n.position.column);
        builder_.br(done);
    } else {
        builder_.call(runtime_abi::failure::conversion_fail,
            {{i32,abi::dtype_code(ir::dtype_of(n.target))},{i32,static_cast<int>(abi::ConversionSubject::value)},
             {i32,static_cast<int>(abi::ConversionReason::out_of_range)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
        builder_.unreachable();
    }
    builder_.block(success);
    auto wide=names_.value("exact.scaled.round");builder_.load(wide,double_type,slot,Align::eight);
    auto value=wide;
    if(n.target.kind==TypeKind::Real16b) {
        auto bits=names_.value("exact.scaled.bf16.bits");value=names_.value("exact.scaled.bf16.value");
        builder_.call(bits,runtime_abi::half_support::real16b_from_float64,{{double_type,wide}});
        builder_.cast(value,CastOp::trunc,{i32,bits},i16);
    } else if(n.target.kind!=TypeKind::Real64) {
        value=names_.value("exact.scaled.narrow");builder_.cast(value,CastOp::fptrunc,{double_type,wide},llvm_type(n.target));
    }
    if(fallible) {
        box.store_tag(symbols_.value(n.out),case_index(result_type,n.target));
        const auto payload=names_.value("exact.scaled.payload");box.payload_slot(payload,symbols_.value(n.out));
        builder_.store({llvm_type(n.target),value},payload,Align::one);
    } else builder_.select(symbols_.value(n.out),"true",{llvm_type(n.target),value},{llvm_type(n.target),value});
    builder_.br(done);builder_.block(done);
}

void NumericEmitter::emit(const ir::ComplexMake& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.type.kind==TypeKind::Com)
        complex_.com_make(symbols_.value(n.out),symbols_.value(n.re),symbols_.value(n.im));
    else
        complex_.make(n.type,symbols_.value(n.out),symbols_.value(n.re),symbols_.value(n.im));
}

void NumericEmitter::emit(const ir::ComplexPart& n,const ir::Instruction&){
    symbols_.set_type(n.out,complex_part_type(n.type));
    if(n.type.kind==TypeKind::Com)
        complex_.com_part(symbols_.value(n.out),symbols_.value(n.value),n.part);
    else
        builder_.extractvalue(symbols_.value(n.out),llvm_type(n.type),symbols_.value(n.value),n.part);
}

void NumericEmitter::emit(const ir::ExactAtom& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto provider=pool_.intern(n.provider);
    const auto node=names_.value("exact.atom.node");
    builder_.call(node,runtime_abi::exact::real_atom,{{ptr,LlvmOperand::global(provider)},{i32,n.opcode}});
    builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::adopt_node),{{ptr,node}});
}

void NumericEmitter::emit(const ir::ExactUnary& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto provider=pool_.intern(n.provider);
    builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::unary),{{ptr,LlvmOperand::global(provider)},{i32,n.opcode},{exact_real_type,symbols_.value(n.input)}});
}

// The runtime rounds to the format itself and returns the result widened to
// double, so narrowing it to float is exact (no double rounding).
void NumericEmitter::emit(const ir::ExactRound& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const bool single=n.type.kind==TypeKind::Real32;
    const auto rounded=single?names_.value("exact.round"):symbols_.value(n.out);
    builder_.call(rounded,double_type,LlvmOperand::global(exact_helper::round),{{exact_real_type,symbols_.value(n.value)},{i32,single?1:0},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    if(single)builder_.cast(symbols_.value(n.out),CastOp::fptrunc,{double_type,rounded},float_type);
    if(!n.slot.empty())
        builder_.store({single?float_type:double_type,symbols_.value(n.out)},
                       LlvmOperand::global(pool_.intern_exact_slot(n.slot,single)),Align::none);
}

void NumericEmitter::emit(const ir::ExactConstant& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const bool single=n.type.kind==TypeKind::Real32;
    builder_.load(symbols_.value(n.out),single?float_type:double_type,
                  LlvmOperand::global(pool_.intern_exact_slot(n.slot,single)),Align::none);
}

// The arguments are pushed one by one onto a runtime list, which the
// operation then consumes.
void NumericEmitter::emit(const ir::ExactOp& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto provider=pool_.intern(n.provider);
    std::string list="null",list_owner;
    for(const auto input:n.inputs){
        const auto next=names_.value("exact.op.args");
        builder_.call(next,ptr,LlvmOperand::global(exact_helper::push),{{ptr,list},{exact_real_type,symbols_.value(input)}});
        if(list_owner.empty()) {
            list_owner=next;
            symbols_.acquire_custom_temporary(list_owner,Type::simple(TypeKind::Bin),"quidra_real_op_drop");
        }
        list=next;
    }
    // The vector address remains unchanged by push; apply takes ownership
    // immediately on entry, before provider rejection or allocation.
    if(!list_owner.empty())symbols_.transfer_temporary(list_owner,Type::simple(TypeKind::Bin));
    builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(exact_helper::apply),{{ptr,LlvmOperand::global(provider)},{i32,n.opcode},{ptr,list}});
    symbols_.acquire_owned(n.out,n.type,symbols_.value(n.out));
}

void NumericEmitter::emit(const ir::Unary& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.op=="int.natural_view") {
        copy_by_add_zero(builder_,symbols_.value(n.out),bare_integer_type,symbols_.value(n.operand));
    }else if(n.op=="int.is_inline") {
        // Compiler-owned range entry predicate: an inline integer word is
        // even. This operation is never exposed as source arithmetic.
        const auto tag=names_.value("for.integer.tag");
        builder_.binary(tag,BinaryOp::and_,bare_integer_type,symbols_.value(n.operand),1);
        builder_.icmp(symbols_.value(n.out),IntPredicate::eq,bare_integer_type,tag,0);
    }else if(n.type.kind==TypeKind::Tensor){
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::unary,{{ptr,symbols_.value(n.operand)},{i32,1},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }else if(n.type.kind==TypeKind::Com){
        complex_.com_negate(symbols_.value(n.out),symbols_.value(n.operand),n.position.line,n.position.column);
    }else if(is_fixed_complex(n.type)){
        complex_.negate(n.type,symbols_.value(n.out),symbols_.value(n.operand));
    }else if(n.op=="not"){
        builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,symbols_.value(n.operand),"true");
    }else if(n.op=="NOT"){
        builder_.binary(symbols_.value(n.out),BinaryOp::xor_,llvm_type(n.type),symbols_.value(n.operand),-1);
    }else if(n.type.kind==TypeKind::Nat){
        // A nat is never negative: its negation is 0 - value, which fails
        // unless the value is 0.
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_sub),{{bare_integer_type,0},{bare_integer_type,symbols_.value(n.operand)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }else if(is_bare_integer(n.type)&&n.inline_proven){
        // An inline word negated is the negated word, and the result is
        // proven inline.
        builder_.binary(symbols_.value(n.out),BinaryOp::sub,NoWrap::nsw,bare_integer_type,0,symbols_.value(n.operand));
    }else if(is_bare_integer(n.type)){
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::neg),{{bare_integer_type,symbols_.value(n.operand)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }else if(n.type.kind==TypeKind::Real){
        builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::neg),{{exact_real_type,symbols_.value(n.operand)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }else if(is_fixed_integer(n.type)){
        const auto ty=llvm_type(n.type);
        const auto width=integer_width(n.type);
        const auto pair=names_.value("neg.pair"),overflow=names_.value("neg.overflow");
        const auto intrinsic=runtime_abi::llvm_intrinsic::with_overflow(true,"sub",width);
        builder_.call(pair,LlvmType::structure(ty,i1),intrinsic,{{ty,0},{ty,symbols_.value(n.operand)}});
        builder_.extractvalue(symbols_.value(n.out),LlvmType::structure(ty,i1),pair,0);
        builder_.extractvalue(overflow,LlvmType::structure(ty,i1),pair,1);
        fail_fast_.fail_if(overflow,"@.code.overflow","@.msg.overflow","neg",n.position.line,n.position.column);
    }else if(n.type.kind==TypeKind::Real16b){
        // The sign bit of the bfloat16 bits.
        builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i16,symbols_.value(n.operand),-32768);
    }else{
        builder_.fneg(symbols_.value(n.out),llvm_type(n.type),symbols_.value(n.operand));
    }
}

void NumericEmitter::emit(const ir::Binary& n,const ir::Instruction& ins){symbols_.set_type(n.out,n.result_type);const auto&ot=n.operand_type;
    if(ot.kind==TypeKind::Com){
        complex_.com_binary(n.op,symbols_.value(n.out),symbols_.value(n.left),symbols_.value(n.right),n.position.line,n.position.column);
        return;
    }
    if(is_fixed_complex(ot)){
        if(n.op=="^")
            complex_.power(ot,symbols_.value(n.out),symbols_.value(n.left),symbols_.value(n.right),
                           scratch_.instruction_slot(ins),n.position.line,n.position.column);
        else
            complex_.binary(n.op,ot,symbols_.value(n.out),symbols_.value(n.left),symbols_.value(n.right),n.position.line,n.position.column);
        return;
    }
    if((n.op=="=="||n.op=="!=")&&ot.kind==TypeKind::Address){builder_.icmp(symbols_.value(n.out),(n.op=="=="?IntPredicate::eq:IntPredicate::ne),ptr,symbols_.value(n.left),symbols_.value(n.right));return;}
    if(n.op=="+"&&ot.kind==TypeKind::String){builder_.call(symbols_.value(n.out),runtime_abi::text::concat2,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)}});return;}
    if((n.op=="=="||n.op=="!=")&&(ot.kind==TypeKind::String||ot.kind==TypeKind::Error)){auto equal="%str.equal."+std::to_string(n.out);/* errors compare their codes, details and causes */builder_.call(equal,ot.kind==TypeKind::Error?runtime_abi::error::equal:runtime_abi::text::equal,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)}});if(n.op=="==")copy_by_xor_false(builder_,symbols_.value(n.out),equal);else builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,equal,"true");return;}
    if((n.op=="=="||n.op=="!=")&&(ot.kind==TypeKind::Union||ot.kind==TypeKind::Tuple||ot.kind==TypeKind::Bin||ot.kind==TypeKind::Array||ot.kind==TypeKind::Class)) {
        const auto eq="%deep.eq."+std::to_string(n.out);
        // LlvmCall borrows its callee and operand text until the call is emitted.
        const auto callee=equality_name(ot);
        const auto left=symbols_.value(n.left),right=symbols_.value(n.right);
        LlvmCall call(i1,callee);
        call.result(eq).argument({llvm_type(ot),left}).argument({llvm_type(ot),right});
        if(holds_reference_fields(ot,layouts_)) {
            call.argument({i64,sites_.line(n.position.line)});
            call.argument({i64,sites_.column(n.position.column)});
        }
        builder_.call(call);
        if(n.op=="==")copy_by_xor_false(builder_,symbols_.value(n.out),eq);
        else builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,eq,"true");
        return;
    }
    if(ot.kind==TypeKind::Real){
        // The operand names outlive the calls that name them.
        const auto left=symbols_.value(n.left),right=symbols_.value(n.right);
        if(n.op=="^"){
            builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::pow),{{exact_real_type,left},{exact_real_type,right},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            return;
        }
        if(n.op=="+"||n.op=="-"||n.op=="*"||n.op=="/"){
            const int opcode=n.op=="+"?abi::exact_binary_opcode::add:n.op=="-"?abi::exact_binary_opcode::subtract:
                             n.op=="*"?abi::exact_binary_opcode::multiply:abi::exact_binary_opcode::divide;
            builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::binary),{{exact_real_type,left},{exact_real_type,right},{i32,opcode},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            return;
        }
        const auto comparison=names_.value("exact.compare");
        builder_.call(comparison,i32,LlvmOperand::global(real_helper::compare),{{exact_real_type,left},{exact_real_type,right},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
        IntPredicate pred;
        if(n.op=="==")pred=IntPredicate::eq;else if(n.op=="!=")pred=IntPredicate::ne;
        else if(n.op=="<")pred=IntPredicate::slt;else if(n.op=="<=")pred=IntPredicate::sle;
        else if(n.op==">")pred=IntPredicate::sgt;else pred=IntPredicate::sge;
        builder_.icmp(symbols_.value(n.out),pred,i32,comparison,0);
        return;
    }
    if(is_bare_integer(ot)){
        // Arbitrary-precision integer words: the helpers take the inline
        // fast paths and call the runtime for the rest.
        const auto left=symbols_.value(n.left),right=symbols_.value(n.right);
        if(ot.kind==TypeKind::Nat&&n.op=="-"){
            // A nat difference fails when it would be negative.
            builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_sub),{{bare_integer_type,left},{bare_integer_type,right},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            return;
        }
        if(n.inline_proven&&(n.op=="+"||n.op=="-")){
            // Inline words are twice their values: their sum and difference
            // are the result's word, proven inline.
            builder_.binary(symbols_.value(n.out),n.op=="+"?BinaryOp::add:BinaryOp::sub,NoWrap::nsw,bare_integer_type,left,right);
            return;
        }
        if(n.inline_proven&&n.op=="*"){
            const auto half=names_.value("int.half");
            builder_.binary(half,BinaryOp::ashr,bare_integer_type,left,1);
            builder_.binary(symbols_.value(n.out),BinaryOp::mul,NoWrap::nsw,bare_integer_type,half,right);
            return;
        }
        if(n.inline_proven&&n.op!="/"&&n.op!="%"&&n.op!="^"){
            // Inline words order like their values.
            IntPredicate pred;
            if(n.op=="==")pred=IntPredicate::eq;else if(n.op=="!=")pred=IntPredicate::ne;
            else if(n.op=="<")pred=IntPredicate::slt;else if(n.op=="<=")pred=IntPredicate::sle;
            else if(n.op==">")pred=IntPredicate::sgt;else pred=IntPredicate::sge;
            builder_.icmp(symbols_.value(n.out),pred,bare_integer_type,left,right);
            return;
        }
        if(n.op=="+"||n.op=="-"||n.op=="*"){
            const auto helper=n.op=="+"?int_helper::add:n.op=="-"?int_helper::sub:int_helper::mul;
            builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(helper),{{bare_integer_type,left},{bare_integer_type,right}});
            return;
        }
        if(n.op=="/"||n.op=="%"||n.op=="^"){
            const auto helper=n.op=="/"?int_helper::div:n.op=="%"?int_helper::rem:int_helper::pow;
            builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(helper),{{bare_integer_type,left},{bare_integer_type,right},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            return;
        }
        const auto helper=n.op=="=="?int_helper::eq:n.op=="!="?int_helper::ne:n.op=="<"?int_helper::lt:
                          n.op=="<="?int_helper::le:n.op==">"?int_helper::gt:int_helper::ge;
        builder_.call(symbols_.value(n.out),i1,LlvmOperand::global(helper),{{bare_integer_type,left},{bare_integer_type,right}});
        return;
    }
    if(is_fixed_integer(ot)){
        const auto ty=llvm_type(ot);
        const auto width=integer_width(ot);
        if(operator_policy::is_bitwise_logic(n.op)){
            const auto instruction=n.op=="AND"?BinaryOp::and_:n.op=="OR"?BinaryOp::or_:BinaryOp::xor_;
            builder_.binary(symbols_.value(n.out),instruction,ty,symbols_.value(n.left),symbols_.value(n.right));
            return;
        }
        if(operator_policy::is_shift(n.op)){
            std::string invalid;
            const auto high=names_.value("shift.high");
            if(is_signed_integer(ot)){
                const auto low=names_.value("shift.low");
                invalid=names_.value("shift.invalid");
                builder_.icmp(low,IntPredicate::slt,ty,symbols_.value(n.right),0);
                builder_.icmp(high,IntPredicate::sge,ty,symbols_.value(n.right),width);
                builder_.binary(invalid,BinaryOp::or_,i1,low,high);
            }else{
                invalid=high;
                builder_.icmp(high,IntPredicate::uge,ty,symbols_.value(n.right),width);
            }
            fail_fast_.fail_if(invalid,"@.code.shift","@.msg.shift","shift",n.position.line,n.position.column);
            const auto instruction=n.op=="<<"?BinaryOp::shl:(is_signed_integer(ot)?BinaryOp::ashr:BinaryOp::lshr);
            builder_.binary(symbols_.value(n.out),instruction,ty,symbols_.value(n.left),symbols_.value(n.right));
            return;
        }
        if(n.op=="^"){
            std::string base=symbols_.value(n.left), exponent=symbols_.value(n.right);
            if(width<64){
                const auto wide_base=names_.value("power.base");
                const auto wide_exponent=names_.value("power.exponent");
                const auto extension=is_signed_integer(ot)?CastOp::sext:CastOp::zext;
                builder_.cast(wide_base,extension,{ty,base},i64);
                builder_.cast(wide_exponent,extension,{ty,exponent},i64);
                base=wide_base;
                exponent=wide_exponent;
            }
            const auto wide_result=names_.value("power.result");
            builder_.call(wide_result,is_signed_integer(ot)?runtime_abi::numeric::integer_pow_signed:runtime_abi::numeric::integer_pow_unsigned,{{i64,base},{i64,exponent},{i32,width},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            if(width<64)
                builder_.cast(symbols_.value(n.out),CastOp::trunc,{i64,wide_result},ty);
            else
                copy_by_add_zero(builder_,symbols_.value(n.out),i64,wide_result);
            return;
        }
        if(n.op=="+"||n.op=="-"||n.op=="*"){
            const auto operation=n.op=="+"?BinaryOp::add:n.op=="-"?BinaryOp::sub:BinaryOp::mul;
            if(n.overflow_proven){
                const auto nowrap=is_signed_integer(ot)?NoWrap::nsw:NoWrap::nuw;
                builder_.binary(symbols_.value(n.out),operation,nowrap,ty,symbols_.value(n.left),symbols_.value(n.right));
                return;
            }
            const auto opname=n.op=="+"?"add":n.op=="-"?"sub":"mul";
            const auto pair=names_.value("arith.pair"),overflow=names_.value("arith.overflow");
            const auto intrinsic=runtime_abi::llvm_intrinsic::with_overflow(is_signed_integer(ot),opname,width);
            builder_.call(pair,LlvmType::structure(ty,i1),intrinsic,{{ty,symbols_.value(n.left)},{ty,symbols_.value(n.right)}});
            builder_.extractvalue(symbols_.value(n.out),LlvmType::structure(ty,i1),pair,0);
            builder_.extractvalue(overflow,LlvmType::structure(ty,i1),pair,1);
            fail_fast_.fail_if(overflow,"@.code.overflow","@.msg.overflow","arith",n.position.line,n.position.column);
            return;
        }
        if(n.op=="/"||n.op=="%"){
            const auto zero=names_.value("arith.zero");
            builder_.icmp(zero,IntPredicate::eq,ty,symbols_.value(n.right),0);
            fail_fast_.fail_if(zero,"@.code.divzero","@.msg.divzero","arith.zero",n.position.line,n.position.column);
            if(is_signed_integer(ot)){
                const std::string min_value=width==64?"-9223372036854775808":std::to_string(-(1LL<<(width-1)));
                const auto ismin=names_.value("arith.min"),isnegone=names_.value("arith.negone"),special=names_.value("arith.special");
                builder_.icmp(ismin,IntPredicate::eq,ty,symbols_.value(n.left),min_value);
                builder_.icmp(isnegone,IntPredicate::eq,ty,symbols_.value(n.right),-1);
                builder_.binary(special,BinaryOp::and_,i1,ismin,isnegone);
                fail_fast_.fail_if(special,"@.code.overflow","@.msg.overflow","arith.div.overflow",n.position.line,n.position.column);
            }
            const auto operation=n.op=="/"?(is_signed_integer(ot)?BinaryOp::sdiv:BinaryOp::udiv)
                                          :(is_signed_integer(ot)?BinaryOp::srem:BinaryOp::urem);
            builder_.binary(symbols_.value(n.out),operation,ty,symbols_.value(n.left),symbols_.value(n.right));
            return;
        }
        IntPredicate p;
        if(n.op=="==")p=IntPredicate::eq;else if(n.op=="!=")p=IntPredicate::ne;
        else if(n.op=="<")p=is_signed_integer(ot)?IntPredicate::slt:IntPredicate::ult;
        else if(n.op=="<=")p=is_signed_integer(ot)?IntPredicate::sle:IntPredicate::ule;
        else if(n.op==">")p=is_signed_integer(ot)?IntPredicate::sgt:IntPredicate::ugt;
        else p=is_signed_integer(ot)?IntPredicate::sge:IntPredicate::uge;
        builder_.icmp(symbols_.value(n.out),p,ty,symbols_.value(n.left),symbols_.value(n.right));
        return;
    }
    if(ot.kind==TypeKind::Real16b){
        // Both operands widened exactly to float; an arithmetic result
        // rounded once back to bfloat16.
        const auto left=names_.value("bf16.left"),right=names_.value("bf16.right");
        builder_.call(left,float_type,LlvmOperand::global(half_helper::bf16_widen),{{i16,symbols_.value(n.left)}});
        builder_.call(right,float_type,LlvmOperand::global(half_helper::bf16_widen),{{i16,symbols_.value(n.right)}});
        if(n.op=="+"||n.op=="-"||n.op=="*"||n.op=="/"||n.op=="^"){
            const auto result=names_.value("bf16.result");
            if(n.op=="^")builder_.call(result,float_type,LlvmOperand::global(power_helper::real32),{{float_type,left},{float_type,right},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            else builder_.binary(result,n.op=="+"?BinaryOp::fadd:n.op=="-"?BinaryOp::fsub:n.op=="*"?BinaryOp::fmul:BinaryOp::fdiv,float_type,left,right);
            builder_.call(symbols_.value(n.out),i16,LlvmOperand::global(half_helper::bf16_narrow),{{float_type,result}});
            return;
        }
        FloatPredicate p;if(n.op=="==")p=FloatPredicate::oeq;else if(n.op=="!=")p=FloatPredicate::une;else if(n.op=="<")p=FloatPredicate::olt;else if(n.op=="<=")p=FloatPredicate::ole;else if(n.op==">")p=FloatPredicate::ogt;else p=FloatPredicate::oge;
        builder_.fcmp(symbols_.value(n.out),p,float_type,left,right);
        return;
    }
    if(ot.kind==TypeKind::Real16&&n.op=="^"){
        // The power of the float operands (exact), rounded once to half.
        const auto left=names_.value("half.left"),right=names_.value("half.right"),result=names_.value("half.power");
        builder_.cast(left,CastOp::fpext,{half_type,symbols_.value(n.left)},float_type);
        builder_.cast(right,CastOp::fpext,{half_type,symbols_.value(n.right)},float_type);
        builder_.call(result,float_type,LlvmOperand::global(power_helper::real32),{{float_type,left},{float_type,right},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
        builder_.cast(symbols_.value(n.out),CastOp::fptrunc,{float_type,result},half_type);
        return;
    }
    if(is_fixed_real(ot)){
        const auto ty=llvm_type(ot);
        if(n.op=="+")builder_.binary(symbols_.value(n.out),BinaryOp::fadd,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="-")builder_.binary(symbols_.value(n.out),BinaryOp::fsub,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="*")builder_.binary(symbols_.value(n.out),BinaryOp::fmul,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="/")builder_.binary(symbols_.value(n.out),BinaryOp::fdiv,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="^")builder_.call(symbols_.value(n.out),ty,LlvmOperand::global(ot.kind==TypeKind::Real32?power_helper::real32:power_helper::real64),{{ty,symbols_.value(n.left)},{ty,symbols_.value(n.right)},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
        else{FloatPredicate p;if(n.op=="==")p=FloatPredicate::oeq;else if(n.op=="!=")p=FloatPredicate::une;else if(n.op=="<")p=FloatPredicate::olt;else if(n.op=="<=")p=FloatPredicate::ole;else if(n.op==">")p=FloatPredicate::ogt;else p=FloatPredicate::oge;builder_.fcmp(symbols_.value(n.out),p,ty,symbols_.value(n.left),symbols_.value(n.right));}
        return;
    }
    if(ot.kind==TypeKind::Bool){if(n.op=="and"||n.op=="or")builder_.binary(symbols_.value(n.out),n.op=="and"?BinaryOp::and_:BinaryOp::or_,i1,symbols_.value(n.left),symbols_.value(n.right));else builder_.icmp(symbols_.value(n.out),(n.op=="=="?IntPredicate::eq:IntPredicate::ne),i1,symbols_.value(n.left),symbols_.value(n.right));}
}

} // namespace quidra::llvm_backend
