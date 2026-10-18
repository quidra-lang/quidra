#include "llvm_backend/complex_arithmetic.hpp"

#include "llvm_backend/half_precision.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/small_rational.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

namespace {

constexpr std::array<const LlvmCallee*, 16> declarations{{
    &runtime_abi::complex_support::text,
    &runtime_abi::complex_support::parse,
    &runtime_abi::complex_support::pow,
    &runtime_abi::complex_support::from_parts,
    &runtime_abi::complex_support::from_integer,
    &runtime_abi::complex_support::from_real,
    &runtime_abi::complex_support::from_com,
    &runtime_abi::complex_support::com_drop,
    &runtime_abi::complex_support::com_make,
    &runtime_abi::complex_support::com_from_float64,
    &runtime_abi::complex_support::com_from_integer,
    &runtime_abi::complex_support::com_binary,
    &runtime_abi::complex_support::com_neg,
    &runtime_abi::complex_support::com_equal,
    &runtime_abi::complex_support::com_text,
    &runtime_abi::complex_support::com_parse,
}};

std::string support_text() {
    std::string out = "\n";
    for (const auto* callee : declarations) append_declaration(out, *callee);
    return out;
}

abi::RealOperation real_operation(const std::string& op) {
    if (op == "+") return abi::RealOperation::addition;
    if (op == "-") return abi::RealOperation::subtraction;
    if (op == "*") return abi::RealOperation::multiplication;
    return abi::RealOperation::division;
}

} // namespace

LlvmType fixed_complex_type(const Type& type) {
    const auto part = llvm_type(complex_part_type(type));
    return LlvmType::structure(part, part);
}

int complex_kind_code(const Type& type) {
    return abi::dtype_code(*numeric_kind_info(type.kind)->dtype);
}

std::span<const LlvmCallee* const> complex_support_declarations() { return declarations; }

std::string_view complex_support_text() {
    static const std::string text = support_text();
    return text;
}

std::string widen_complex_part(LlvmBuilder& builder, TemporaryNames& names, const Type& part,
                               const std::string& value) {
    const auto out = names.value("complex.wide");
    switch (part.kind) {
        case TypeKind::Real64:
            builder.binary(out, BinaryOp::fadd, double_type, value, "-0.0");
            return out;
        case TypeKind::Real16b: {
            const auto single = names.value("complex.bf16");
            builder.call(single, float_type, LlvmOperand::global(half_helper::bf16_widen),
                         {{i16, value}});
            builder.cast(out, CastOp::fpext, {float_type, single}, double_type);
            return out;
        }
        default:
            builder.cast(out, CastOp::fpext, {llvm_type(part), value}, double_type);
            return out;
    }
}

std::string emit_complex_conversion(LlvmBuilder& builder,TemporaryNames& names,
                                    const Type& source,const Type& target,
                                                       const std::string& value,const std::string& slot){
    if(is_bare_integer(source)){
        const auto exact=names.value("cast.complex.exact");
        builder.call(exact,exact_real_type,LlvmOperand::global(real_helper::from_bigint),
                     {{bare_integer_type,value}});
        const auto result=emit_complex_conversion(builder,names,Type::simple(TypeKind::Real),target,exact,slot);
        builder.call(void_type,LlvmOperand::global(real_helper::release),{{exact_real_type,exact}});
        return result;
    }
    if(target.kind==TypeKind::Com && source.kind==TypeKind::Com){
        builder.call(runtime_abi::memory::managed_retain,{{ptr,value}});
        return value;
    }
    if(target.kind==TypeKind::Com){
        const auto out=names.value("cast.com");
        if(source.kind==TypeKind::Real){
            const auto n=names.value("cast.com.n"),d=names.value("cast.com.d");
            builder.extractvalue(n,exact_real_type,value,0);
            builder.extractvalue(d,exact_real_type,value,1);
            builder.call(out,runtime_abi::complex_support::com_make,{{i64,n},{i64,d},{i64,0},{i64,1}});
        }else if(is_fixed_integer(source)){
            std::string widened=value;
            if(integer_width(source)<64){
                widened=names.value("cast.com.widen");
                builder.cast(widened,is_signed_integer(source)?CastOp::sext:CastOp::zext,{llvm_type(source),value},i64);
            }
            builder.call(out,runtime_abi::complex_support::com_from_integer,
                          {{i64,widened},{i1,is_signed_integer(source)?"true":"false"}});
        }else{
            std::string re=value,im="0.0";
            if(is_fixed_complex(source)){
                const auto part=complex_part_type(source);
                const auto raw_re=names.value("cast.com.re"),raw_im=names.value("cast.com.im");
                builder.extractvalue(raw_re,llvm_type(source),value,0);
                builder.extractvalue(raw_im,llvm_type(source),value,1);
                re=widen_complex_part(builder,names,part,raw_re);
                im=widen_complex_part(builder,names,part,raw_im);
            }else{
                re=widen_complex_part(builder,names,source,value);
            }
            builder.call(out,runtime_abi::complex_support::com_from_float64,{{double_type,re},{double_type,im}});
        }
        return out;
    }
    const auto ok=names.value("cast.complex.ok");
    const int kind=complex_kind_code(target);
    if(source.kind==TypeKind::Com){
        builder.call(ok,runtime_abi::complex_support::from_com,{{ptr,value},{i32,kind},{ptr,slot}});
    }else if(source.kind==TypeKind::Real){
        const auto n=names.value("cast.complex.n"),d=names.value("cast.complex.d");
        builder.extractvalue(n,exact_real_type,value,0);
        builder.extractvalue(d,exact_real_type,value,1);
        builder.call(ok,runtime_abi::complex_support::from_real,{{i64,n},{i64,d},{i32,kind},{ptr,slot}});
    }else if(is_fixed_integer(source)){
        std::string widened=value;
        if(integer_width(source)<64){
            widened=names.value("cast.complex.widen");
            builder.cast(widened,is_signed_integer(source)?CastOp::sext:CastOp::zext,{llvm_type(source),value},i64);
        }
        builder.call(ok,runtime_abi::complex_support::from_integer,
                      {{i64,widened},{i1,is_signed_integer(source)?"true":"false"},{i32,kind},{ptr,slot}});
    }else if(is_fixed_complex(source)){
        const auto part=complex_part_type(source);
        const auto raw_re=names.value("cast.complex.re"),raw_im=names.value("cast.complex.im");
        builder.extractvalue(raw_re,llvm_type(source),value,0);
        builder.extractvalue(raw_im,llvm_type(source),value,1);
        const auto re=widen_complex_part(builder,names,part,raw_re);
        const auto im=widen_complex_part(builder,names,part,raw_im);
        builder.call(ok,runtime_abi::complex_support::from_parts,{{double_type,re},{double_type,im},{i32,kind},{ptr,slot}});
    }else{
        const auto re=widen_complex_part(builder,names,source,value);
        builder.call(ok,runtime_abi::complex_support::from_parts,{{double_type,re},{double_type,"0.0"},{i32,kind},{ptr,slot}});
    }
    return ok;
}

std::string emit_complex_text(LlvmBuilder& builder, TemporaryNames& names, const Type& type,
                              const std::string& value, int integer_width, int fractional,
                              int significant, bool zero) {
    const auto text = names.value("complex.text");
    if (type.kind == TypeKind::Com) {
        builder.call(text, runtime_abi::complex_support::com_text,
                     {{ptr, value},
                      {i32, significant >= 0 ? significant : abi::exact_real_canonical_text}});
        return text;
    }
    const auto part = complex_part_type(type);
    const auto re = names.value("complex.text.re"), im = names.value("complex.text.im");
    builder.extractvalue(re, fixed_complex_type(type), value, 0);
    builder.extractvalue(im, fixed_complex_type(type), value, 1);
    const auto wide_re = widen_complex_part(builder, names, part, re);
    const auto wide_im = widen_complex_part(builder, names, part, im);
    builder.call(text, runtime_abi::complex_support::text,
                 {{double_type, wide_re}, {double_type, wide_im}, {i32, complex_kind_code(type)},
                  {i32, integer_width}, {i32, fractional}, {i32, significant},
                  {i32, zero ? 1 : 0}});
    return text;
}

std::string ComplexArithmetic::part(const Type& type, const std::string& value, unsigned index) {
    const auto out = names_.value(index == 0 ? "complex.re" : "complex.im");
    builder_.extractvalue(out, fixed_complex_type(type), value, index);
    return out;
}

void ComplexArithmetic::make(const Type& type, const std::string& out, const std::string& re,
                             const std::string& im) {
    const auto aggregate = fixed_complex_type(type);
    const auto part_type = llvm_type(complex_part_type(type));
    const auto first = names_.value("complex.make");
    builder_.insertvalue(first, aggregate, "undef", {part_type, re}, 0);
    builder_.insertvalue(out, aggregate, first, {part_type, im}, 1);
}

std::string ComplexArithmetic::widen(const Type& part, const std::string& value) {
    return widen_complex_part(builder_, names_, part, value);
}

std::string narrow_complex_part(LlvmBuilder& builder, TemporaryNames& names,
                                const Type& part, const std::string& value) {
    if (part.kind == TypeKind::Real16)
        return real16_from_float64(builder, names, value);
    const auto out = names.value("complex.part");
    switch (part.kind) {
        case TypeKind::Real64:
            builder.binary(out, BinaryOp::fadd, double_type, value, "-0.0");
            return out;
        case TypeKind::Real16b: {
            const auto single = names.value("complex.single");
            builder.cast(single, CastOp::fptrunc, {double_type, value}, float_type);
            builder.call(out, i16, LlvmOperand::global(half_helper::bf16_narrow),
                          {{float_type, single}});
            return out;
        }
        default:
            builder.cast(out, CastOp::fptrunc, {double_type, value}, llvm_type(part));
            return out;
    }
}

std::string ComplexArithmetic::narrow(const Type& part, const std::string& value) {
    return narrow_complex_part(builder_,names_,part,value);
}

std::string ComplexArithmetic::op(BinaryOp operation, const Type& part, const std::string& left,
                                  const std::string& right) {
    const auto out = names_.value("complex.op");
    if (part.kind != TypeKind::Real16b) {
        builder_.binary(out, operation, llvm_type(part), left, right);
        return out;
    }
    // real16b: the operation in binary32 on the exact values, rounded once.
    const auto a = names_.value("complex.bf16.a"), b = names_.value("complex.bf16.b");
    const auto r = names_.value("complex.bf16.r");
    builder_.call(a, float_type, LlvmOperand::global(half_helper::bf16_widen), {{i16, left}});
    builder_.call(b, float_type, LlvmOperand::global(half_helper::bf16_widen), {{i16, right}});
    builder_.binary(r, operation, float_type, a, b);
    builder_.call(out, i16, LlvmOperand::global(half_helper::bf16_narrow), {{float_type, r}});
    return out;
}

std::string ComplexArithmetic::nonfinite(const Type& part, const std::string& value) {
    const auto bad = names_.value("complex.bad");
    if (part.kind == TypeKind::Real16b) {
        const auto exponent = names_.value("complex.exponent");
        builder_.binary(exponent, BinaryOp::and_, i16, value, 32767);
        builder_.icmp(bad, IntPredicate::ugt, i16, exponent, 32640);
        return bad;
    }
    builder_.fcmp(bad, FloatPredicate::uno, llvm_type(part), value, value);
    return bad;
}

std::string ComplexArithmetic::any(const std::vector<std::string>& conditions) {
    auto result = conditions.front();
    for (std::size_t i = 1; i < conditions.size(); ++i) {
        const auto joined = names_.value("complex.any");
        builder_.binary(joined, BinaryOp::or_, i1, result, conditions[i]);
        result = joined;
    }
    return result;
}

LlvmType ComplexArithmetic::comparable_type(const Type& part) {
    return part.kind == TypeKind::Real16b ? float_type : llvm_type(part);
}

std::string ComplexArithmetic::comparable(const Type& part, const std::string& value) {
    if (part.kind != TypeKind::Real16b) return value;
    const auto out = names_.value("complex.bf16.value");
    builder_.call(out, float_type, LlvmOperand::global(half_helper::bf16_widen), {{i16, value}});
    return out;
}

std::string ComplexArithmetic::magnitude(const Type& part, const std::string& value) {
    const auto type = comparable_type(part);
    const auto x = comparable(part, value);
    const auto negative = names_.value("complex.negative"), negated = names_.value("complex.negated");
    const auto out = names_.value("complex.magnitude");
    builder_.fcmp(negative, FloatPredicate::olt, type, x, "0.0");
    builder_.fneg(negated, type, x);
    builder_.select(out, negative, {type, negated}, {type, x});
    return out;
}

void ComplexArithmetic::fail_if(const std::string& bad, const Type& type, int operation,
                                const std::string& zero_divisor, std::uint32_t line,
                                std::uint32_t column) {
    const auto fail = names_.label("complex.fail"), ok = names_.label("complex.ok");
    builder_.br(bad, fail, ok);
    builder_.block(fail);
    builder_.call(runtime_abi::finite_support::real_fail,
                  {{i32, complex_kind_code(type)}, {i32, operation}, {i1, zero_divisor},
                   {i64, sites_.line(line)}, {i64, sites_.column(column)}});
    builder_.unreachable();
    builder_.block(ok);
}

void ComplexArithmetic::binary(const std::string& op_name, const Type& type,
                               const std::string& out, const std::string& left,
                               const std::string& right, std::uint32_t line,
                               std::uint32_t column) {
    const auto part_type = complex_part_type(type);
    const auto a = part(type, left, 0), b = part(type, left, 1);
    const auto c = part(type, right, 0), d = part(type, right, 1);
    if (op_name == "==" || op_name == "!=") {
        const auto ctype = comparable_type(part_type);
        const auto re = names_.value("complex.eq.re"), im = names_.value("complex.eq.im");
        // Operands are emitted one statement at a time: the order of the
        // names is the order of these lines.
        const auto left_re = comparable(part_type, a);
        const auto right_re = comparable(part_type, c);
        const auto left_im = comparable(part_type, b);
        const auto right_im = comparable(part_type, d);
        builder_.fcmp(re, FloatPredicate::oeq, ctype, left_re, right_re);
        builder_.fcmp(im, FloatPredicate::oeq, ctype, left_im, right_im);
        if (op_name == "==") {
            builder_.binary(out, BinaryOp::and_, i1, re, im);
        } else {
            const auto equal = names_.value("complex.eq");
            builder_.binary(equal, BinaryOp::and_, i1, re, im);
            builder_.binary(out, BinaryOp::xor_, i1, equal, "true");
        }
        return;
    }
    if (op_name == "/") {
        divide(type, out, left, right, line, column);
        return;
    }
    std::string re, im;
    if (op_name == "+" || op_name == "-") {
        const auto operation = op_name == "+" ? BinaryOp::fadd : BinaryOp::fsub;
        re = op(operation, part_type, a, c);
        im = op(operation, part_type, b, d);
    } else if (op_name == "*") {
        // (ac - bd) + (ad + bc)i, each operation rounded once.
        const auto ac = op(BinaryOp::fmul, part_type, a, c);
        const auto bd = op(BinaryOp::fmul, part_type, b, d);
        const auto ad = op(BinaryOp::fmul, part_type, a, d);
        const auto bc = op(BinaryOp::fmul, part_type, b, c);
        re = op(BinaryOp::fsub, part_type, ac, bd);
        im = op(BinaryOp::fadd, part_type, ad, bc);
    } else {
        throw std::logic_error("unsupported complex operator " + op_name);
    }
    // A non-finite product or sum makes the result non-finite.
    fail_if(any({nonfinite(part_type, re), nonfinite(part_type, im)}), type,
            static_cast<int>(real_operation(op_name)), "false", line, column);
    make(type, out, re, im);
}

// Smith's algorithm: with |c| >= |d|, r = d/c, t = c + d*r and
// (a + bi)/(c + di) = ((a + b*r) + (b - a*r)i)/t; otherwise the same with
// the roles of c and d exchanged. Every intermediate value is tested, since
// a division by an infinity would hide an overflow.
void ComplexArithmetic::divide(const Type& type, const std::string& out, const std::string& left,
                               const std::string& right, std::uint32_t line,
                               std::uint32_t column) {
    const auto p = complex_part_type(type);
    const auto a = part(type, left, 0), b = part(type, left, 1);
    const auto c = part(type, right, 0), d = part(type, right, 1);
    const auto ctype = comparable_type(p);
    const auto c_zero = names_.value("complex.c.zero"), d_zero = names_.value("complex.d.zero");
    const auto zero_divisor = names_.value("complex.zero.divisor");
    const auto c_value = comparable(p, c);
    const auto d_value = comparable(p, d);
    builder_.fcmp(c_zero, FloatPredicate::oeq, ctype, c_value, "0.0");
    builder_.fcmp(d_zero, FloatPredicate::oeq, ctype, d_value, "0.0");
    builder_.binary(zero_divisor, BinaryOp::and_, i1, c_zero, d_zero);
    const auto c_magnitude = magnitude(p, c);
    const auto d_magnitude = magnitude(p, d);
    const auto c_wide = names_.value("complex.c.wider");
    builder_.fcmp(c_wide, FloatPredicate::oge, ctype, c_magnitude, d_magnitude);
    const auto by_c = names_.label("complex.div.c"), by_d = names_.label("complex.div.d");
    const auto done = names_.label("complex.div.done");
    builder_.br(c_wide, by_c, by_d);

    const auto part_type = llvm_type(p);
    struct Branch {
        std::string re, im, bad;
    };
    const auto branch = [&](bool real_wider) {
        Branch out_branch;
        std::vector<std::string> checks;
        const auto tested = [&](const std::string& value) {
            checks.push_back(nonfinite(p, value));
            return value;
        };
        if (real_wider) {
            const auto r = tested(op(BinaryOp::fdiv, p, d, c));
            const auto t = tested(op(BinaryOp::fadd, p, c, tested(op(BinaryOp::fmul, p, d, r))));
            const auto re_n = tested(op(BinaryOp::fadd, p, a, tested(op(BinaryOp::fmul, p, b, r))));
            const auto im_n = tested(op(BinaryOp::fsub, p, b, tested(op(BinaryOp::fmul, p, a, r))));
            out_branch.re = tested(op(BinaryOp::fdiv, p, re_n, t));
            out_branch.im = tested(op(BinaryOp::fdiv, p, im_n, t));
        } else {
            const auto r = tested(op(BinaryOp::fdiv, p, c, d));
            const auto t = tested(op(BinaryOp::fadd, p, tested(op(BinaryOp::fmul, p, c, r)), d));
            const auto re_n = tested(op(BinaryOp::fadd, p, tested(op(BinaryOp::fmul, p, a, r)), b));
            const auto im_n = tested(op(BinaryOp::fsub, p, tested(op(BinaryOp::fmul, p, b, r)), a));
            out_branch.re = tested(op(BinaryOp::fdiv, p, re_n, t));
            out_branch.im = tested(op(BinaryOp::fdiv, p, im_n, t));
        }
        out_branch.bad = any(checks);
        return out_branch;
    };
    builder_.block(by_c);
    const auto first = branch(true);
    builder_.br(done);
    builder_.block(by_d);
    const auto second = branch(false);
    builder_.br(done);
    builder_.block(done);
    const auto re = names_.value("complex.div.re"), im = names_.value("complex.div.im");
    const auto bad = names_.value("complex.div.bad");
    builder_.phi(re, part_type, {{first.re, by_c}, {second.re, by_d}});
    builder_.phi(im, part_type, {{first.im, by_c}, {second.im, by_d}});
    builder_.phi(bad, i1, {{first.bad, by_c}, {second.bad, by_d}});
    fail_if(bad, type, static_cast<int>(abi::RealOperation::division), zero_divisor, line, column);
    make(type, out, re, im);
}

void ComplexArithmetic::power(const Type& type, const std::string& out, const std::string& left,
                              const std::string& right, const std::string& slot,
                              std::uint32_t line, std::uint32_t column) {
    const auto p = complex_part_type(type);
    const auto a = widen(p, part(type, left, 0)), b = widen(p, part(type, left, 1));
    const auto c = widen(p, part(type, right, 0)), d = widen(p, part(type, right, 1));
    builder_.call(runtime_abi::complex_support::pow,
                  {{double_type, a}, {double_type, b}, {double_type, c}, {double_type, d},
                   {i32, complex_kind_code(type)}, {ptr, slot}, {i64, sites_.line(line)},
                   {i64, sites_.column(column)}});
    load_parts(type, out, slot);
}

void load_complex_parts(LlvmBuilder& builder, TemporaryNames& names, const Type& type,
                        const std::string& out, const std::string& slot) {
    const auto p=complex_part_type(type);
    const auto re=names.value("complex.slot.re"),im=names.value("complex.slot.im");
    const auto im_slot=names.value("complex.slot.im.address");
    builder.load(re,double_type,slot,Align::eight);
    builder.getelementptr(im_slot,Inbounds::yes,i8,slot,{{i64,8}});
    builder.load(im,double_type,im_slot,Align::eight);
    const auto narrow_re=narrow_complex_part(builder,names,p,re);
    const auto narrow_im=narrow_complex_part(builder,names,p,im);
    const auto first=names.value("complex.make");
    builder.insertvalue(first,fixed_complex_type(type),"undef",{llvm_type(p),narrow_re},0);
    builder.insertvalue(out,fixed_complex_type(type),first,{llvm_type(p),narrow_im},1);
}

void ComplexArithmetic::load_parts(const Type& type, const std::string& out,
                                   const std::string& slot) {
    load_complex_parts(builder_,names_,type,out,slot);
}

void ComplexArithmetic::negate(const Type& type, const std::string& out,
                               const std::string& value) {
    const auto p = complex_part_type(type);
    const auto re = part(type, value, 0), im = part(type, value, 1);
    const auto negated_re = names_.value("complex.neg.re");
    const auto negated_im = names_.value("complex.neg.im");
    if (p.kind == TypeKind::Real16b) {
        builder_.binary(negated_re, BinaryOp::xor_, i16, re, -32768);
        builder_.binary(negated_im, BinaryOp::xor_, i16, im, -32768);
    } else {
        builder_.fneg(negated_re, llvm_type(p), re);
        builder_.fneg(negated_im, llvm_type(p), im);
    }
    make(type, out, negated_re, negated_im);
}

void ComplexArithmetic::com_binary(const std::string& op_name, const std::string& out,
                                   const std::string& left, const std::string& right,
                                   std::uint32_t line, std::uint32_t column) {
    if (op_name == "==" || op_name == "!=") {
        if (op_name == "==") {
            builder_.call(out, runtime_abi::complex_support::com_equal,
                          {{ptr, left}, {ptr, right}});
        } else {
            const auto equal = names_.value("com.equal");
            builder_.call(equal, runtime_abi::complex_support::com_equal,
                          {{ptr, left}, {ptr, right}});
            builder_.binary(out, BinaryOp::xor_, i1, equal, "true");
        }
        return;
    }
    const int opcode = op_name == "+"   ? abi::exact_binary_opcode::add
                       : op_name == "-" ? abi::exact_binary_opcode::subtract
                       : op_name == "*" ? abi::exact_binary_opcode::multiply
                                        : abi::exact_binary_opcode::divide;
    builder_.call(out, runtime_abi::complex_support::com_binary,
                  {{ptr, left}, {ptr, right}, {i32, opcode}, {i64, sites_.line(line)},
                   {i64, sites_.column(column)}});
}

void ComplexArithmetic::com_negate(const std::string& out, const std::string& value,
                                   std::uint32_t line, std::uint32_t column) {
    builder_.call(out, runtime_abi::complex_support::com_neg,
                  {{ptr, value}, {i64, sites_.line(line)}, {i64, sites_.column(column)}});
}

void ComplexArithmetic::com_make(const std::string& out, const std::string& re,
                                 const std::string& im) {
    const auto rn = names_.value("com.re.n"), rd = names_.value("com.re.d");
    const auto in = names_.value("com.im.n"), id = names_.value("com.im.d");
    builder_.extractvalue(rn, exact_real_type, re, 0);
    builder_.extractvalue(rd, exact_real_type, re, 1);
    builder_.extractvalue(in, exact_real_type, im, 0);
    builder_.extractvalue(id, exact_real_type, im, 1);
    builder_.call(out, runtime_abi::complex_support::com_make,
                  {{i64, rn}, {i64, rd}, {i64, in}, {i64, id}});
}

void ComplexArithmetic::com_part(const std::string& out, const std::string& value,
                                 unsigned index) {
    std::string address = value;
    if (index != 0) {
        address = names_.value("com.part.address");
        builder_.getelementptr(address, Inbounds::yes, i8, value,
                               {{i64, abi::complex_box_layout::imaginary_offset}});
    }
    builder_.load(out, exact_real_type, address, Align::eight);
}

} // namespace quidra::llvm_backend
