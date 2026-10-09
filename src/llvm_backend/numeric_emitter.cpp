#include "llvm_backend/numeric_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/real_power.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "operator_policy.hpp"
#include "quidra/abi/exact_codes.hpp"

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

void NumericEmitter::emit(const ir::ExactAtom& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto provider=pool_.intern(n.provider);
    if(n.type.kind==TypeKind::Real){
        const auto node=names_.value("exact.atom.node");
        builder_.call(node,runtime_abi::exact::real_atom,{{ptr,LlvmOperand::global(provider)},{i32,n.opcode}});
        builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::adopt_node),{{ptr,node}});
    }else{
        const auto evaluated=names_.value("exact.atom.f64");
        builder_.call(evaluated,runtime_abi::exact::real_atom_float64,{{ptr,LlvmOperand::global(provider)},{i32,n.opcode}});
        if(n.type.kind==TypeKind::Real32)
            builder_.cast(symbols_.value(n.out),CastOp::fptrunc,{double_type,evaluated},float_type);
        else
            copy_by_leading_zero_fadd(builder_,symbols_.value(n.out),double_type,evaluated);
    }
}

void NumericEmitter::emit(const ir::ExactUnary& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto provider=pool_.intern(n.provider);
    builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::unary),{{ptr,LlvmOperand::global(provider)},{i32,n.opcode},{exact_real_type,symbols_.value(n.input)}});
}

void NumericEmitter::emit(const ir::Unary& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.type.kind==TypeKind::Tensor){
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::unary,{{ptr,symbols_.value(n.operand)},{i32,1},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(n.op=="not"){
        builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,symbols_.value(n.operand),"true");
    }else if(n.op=="NOT"){
        builder_.binary(symbols_.value(n.out),BinaryOp::xor_,llvm_type(n.type),symbols_.value(n.operand),-1);
    }else if(n.type.kind==TypeKind::Nat){
        // A nat is never negative: its negation is 0 - value, which fails
        // unless the value is 0.
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_sub),{{bare_integer_type,0},{bare_integer_type,symbols_.value(n.operand)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(is_bare_integer(n.type)&&n.inline_proven){
        // An inline word negated is the negated word, and the result is
        // proven inline.
        builder_.binary(symbols_.value(n.out),BinaryOp::sub,NoWrap::nsw,bare_integer_type,0,symbols_.value(n.operand));
    }else if(is_bare_integer(n.type)){
        builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::neg),{{bare_integer_type,symbols_.value(n.operand)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(n.type.kind==TypeKind::Real){
        builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::neg),{{exact_real_type,symbols_.value(n.operand)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else if(is_fixed_integer(n.type)){
        const auto ty=llvm_type(n.type);
        const auto width=integer_width(n.type);
        const auto pair=names_.value("neg.pair"),overflow=names_.value("neg.overflow");
        const auto intrinsic=runtime_abi::llvm_intrinsic::with_overflow(true,"sub",width);
        builder_.call(pair,LlvmType::structure(ty,i1),intrinsic,{{ty,0},{ty,symbols_.value(n.operand)}});
        builder_.extractvalue(symbols_.value(n.out),LlvmType::structure(ty,i1),pair,0);
        builder_.extractvalue(overflow,LlvmType::structure(ty,i1),pair,1);
        fail_fast_.fail_if(overflow,"@.code.overflow","@.msg.overflow","neg",n.line,n.column);
    }else{
        builder_.fneg(symbols_.value(n.out),llvm_type(n.type),symbols_.value(n.operand));
    }
}

void NumericEmitter::emit(const ir::Binary& n,const ir::Instruction&){symbols_.set_type(n.out,n.result_type);const auto&ot=n.operand_type;
    if((n.op=="=="||n.op=="!=")&&ot.kind==TypeKind::Address){builder_.icmp(symbols_.value(n.out),(n.op=="=="?IntPredicate::eq:IntPredicate::ne),ptr,symbols_.value(n.left),symbols_.value(n.right));return;}
    if(n.op=="+"&&ot.kind==TypeKind::String){builder_.call(symbols_.value(n.out),runtime_abi::text::concat2,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)}});return;}
    if((n.op=="=="||n.op=="!=")&&(ot.kind==TypeKind::String||ot.kind==TypeKind::Error)){auto equal="%str.equal."+std::to_string(n.out);builder_.call(equal,runtime_abi::text::equal,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)}});if(n.op=="==")copy_by_xor_false(builder_,symbols_.value(n.out),equal);else builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,equal,"true");return;}
    if((n.op=="=="||n.op=="!=")&&(ot.kind==TypeKind::Bin||ot.kind==TypeKind::Array||ot.kind==TypeKind::Class)){auto eq="%deep.eq."+std::to_string(n.out);builder_.call(eq,i1,equality_name(ot),{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)}});if(n.op=="==")copy_by_xor_false(builder_,symbols_.value(n.out),eq);else builder_.binary(symbols_.value(n.out),BinaryOp::xor_,i1,eq,"true");return;}
    if(ot.kind==TypeKind::Real){
        // The operand names outlive the calls that name them.
        const auto left=symbols_.value(n.left),right=symbols_.value(n.right);
        if(n.op=="^"){
            builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::pow),{{exact_real_type,left},{exact_real_type,right},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
            return;
        }
        if(n.op=="+"||n.op=="-"||n.op=="*"||n.op=="/"){
            const int opcode=n.op=="+"?abi::exact_binary_opcode::add:n.op=="-"?abi::exact_binary_opcode::subtract:
                             n.op=="*"?abi::exact_binary_opcode::multiply:abi::exact_binary_opcode::divide;
            builder_.call(symbols_.value(n.out),exact_real_type,LlvmOperand::global(real_helper::binary),{{exact_real_type,left},{exact_real_type,right},{i32,opcode},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
            return;
        }
        const auto comparison=names_.value("exact.compare");
        builder_.call(comparison,i32,LlvmOperand::global(real_helper::compare),{{exact_real_type,left},{exact_real_type,right},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
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
            builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(int_helper::nat_sub),{{bare_integer_type,left},{bare_integer_type,right},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
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
            builder_.call(symbols_.value(n.out),bare_integer_type,LlvmOperand::global(helper),{{bare_integer_type,left},{bare_integer_type,right},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
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
            fail_fast_.fail_if(invalid,"@.code.shift","@.msg.shift","shift",n.line,n.column);
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
            builder_.call(wide_result,is_signed_integer(ot)?runtime_abi::numeric::integer_pow_signed:runtime_abi::numeric::integer_pow_unsigned,{{i64,base},{i64,exponent},{i32,width},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
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
            fail_fast_.fail_if(overflow,"@.code.overflow","@.msg.overflow","arith",n.line,n.column);
            return;
        }
        if(n.op=="/"||n.op=="%"){
            const auto zero=names_.value("arith.zero");
            builder_.icmp(zero,IntPredicate::eq,ty,symbols_.value(n.right),0);
            fail_fast_.fail_if(zero,"@.code.divzero","@.msg.divzero","arith.zero",n.line,n.column);
            if(is_signed_integer(ot)){
                const std::string min_value=width==64?"-9223372036854775808":std::to_string(-(1LL<<(width-1)));
                const auto ismin=names_.value("arith.min"),isnegone=names_.value("arith.negone"),special=names_.value("arith.special");
                builder_.icmp(ismin,IntPredicate::eq,ty,symbols_.value(n.left),min_value);
                builder_.icmp(isnegone,IntPredicate::eq,ty,symbols_.value(n.right),-1);
                builder_.binary(special,BinaryOp::and_,i1,ismin,isnegone);
                fail_fast_.fail_if(special,"@.code.overflow","@.msg.overflow","arith.div.overflow",n.line,n.column);
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
    if(is_fixed_real(ot)){
        const auto ty=llvm_type(ot);
        if(n.op=="+")builder_.binary(symbols_.value(n.out),BinaryOp::fadd,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="-")builder_.binary(symbols_.value(n.out),BinaryOp::fsub,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="*")builder_.binary(symbols_.value(n.out),BinaryOp::fmul,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="/")builder_.binary(symbols_.value(n.out),BinaryOp::fdiv,ty,symbols_.value(n.left),symbols_.value(n.right));
        else if(n.op=="^")builder_.call(symbols_.value(n.out),ty,LlvmOperand::global(ot.kind==TypeKind::Real32?power_helper::real32:power_helper::real64),{{ty,symbols_.value(n.left)},{ty,symbols_.value(n.right)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        else{FloatPredicate p;if(n.op=="==")p=FloatPredicate::oeq;else if(n.op=="!=")p=FloatPredicate::une;else if(n.op=="<")p=FloatPredicate::olt;else if(n.op=="<=")p=FloatPredicate::ole;else if(n.op==">")p=FloatPredicate::ogt;else p=FloatPredicate::oge;builder_.fcmp(symbols_.value(n.out),p,ty,symbols_.value(n.left),symbols_.value(n.right));}
        return;
    }
    if(ot.kind==TypeKind::Bool){if(n.op=="and"||n.op=="or")builder_.binary(symbols_.value(n.out),n.op=="and"?BinaryOp::and_:BinaryOp::or_,i1,symbols_.value(n.left),symbols_.value(n.right));else builder_.icmp(symbols_.value(n.out),(n.op=="=="?IntPredicate::eq:IntPredicate::ne),i1,symbols_.value(n.left),symbols_.value(n.right));}
}

} // namespace quidra::llvm_backend
