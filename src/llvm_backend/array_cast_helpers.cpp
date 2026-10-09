#include "llvm_backend/array_cast_helpers.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/ir/dtype.hpp"

#include <stdexcept>
#include <variant>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

std::string array_cast_name(const Type&from,const Type&to){return "@"+std::string(runtime_abi::generated_helper_prefix::array_cast)+type_identifier(from)+"_to_"+type_identifier(to);}
std::string array_cast_validate_name(const Type&from,const Type&to){return "@"+std::string(runtime_abi::generated_helper_prefix::array_cast_validate)+type_identifier(from)+"_to_"+type_identifier(to);}

namespace {

void collect_array_cast_pair(const Type& source, const Type& target,
                             std::map<std::string,ArrayCastPair>& pairs) {
    if (source.kind != TypeKind::Array || target.kind != TypeKind::Array ||
        source.length != target.length) {
        throw std::logic_error("array numeric cast must preserve array structure");
    }
    if (source.first->kind == TypeKind::Array) {
        if (target.first->kind != TypeKind::Array)
            throw std::logic_error("array numeric cast nesting mismatch");
        collect_array_cast_pair(*source.first, *target.first, pairs);
    } else if (target.first->kind == TypeKind::Array) {
        throw std::logic_error("array numeric cast nesting mismatch");
    }
    pairs.emplace(type_identifier(source) + "_to_" + type_identifier(target),
                  ArrayCastPair{source,target});
}

} // namespace

std::map<std::string,ArrayCastPair> collect_array_cast_pairs(const ir::Module& module) {
    std::map<std::string,ArrayCastPair> pairs;
    for (const auto& function : module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (const auto* cast = std::get_if<ir::ArrayNumericCast>(&instruction))
                    collect_array_cast_pair(cast->source_type, cast->target_type, pairs);
    return pairs;
}

std::string emit_array_cast_validator(const Type& source, const Type& target,
                                      const ArrayLayoutPolicy& array_layout) {
    if (source.kind != TypeKind::Array || target.kind != TypeKind::Array ||
        source.length != target.length) {
        throw std::logic_error("invalid array numeric cast validator types");
    }
    const auto& source_element = *source.first;
    const auto& target_element = *target.first;
    const bool nested = source_element.kind == TypeKind::Array;
    if (nested != (target_element.kind == TypeKind::Array))
        throw std::logic_error("array numeric cast validator nesting mismatch");
    const bool source_fixed = is_fixed_array(source);
    const bool target_fixed = is_fixed_array(target);
    if (source_fixed != target_fixed)
        throw std::logic_error("array numeric cast validator must preserve static dimensions");

    const auto source_stride = array_element_stride(source,array_layout);
    const auto source_offset = source_fixed ? 0 : abi::array_layout::payload_offset;
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(i1, array_cast_validate_name(source,target))
                 .parameter(ptr, "", "%src").parameter(i64, "", "%line").parameter(i64, "", "%column"));
    b.block("entry");
    b.icmp("%null", IntPredicate::eq, ptr, "%src", "null");
    b.br("%null", "fail", "validate.body");
    b.block("validate.body");
    if (source_fixed) copy_by_leading_zero_add(b, "%len", i64, source.length);
    else b.load("%len", i64, "%src", Align::one);
    b.br("cond");
    b.block("cond");
    b.phi("%i", i64, {{0, "validate.body"}, {"%i.next", "advance"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", "%len");
    b.br("%more", "body", "done");
    b.block("body");
    b.binary("%src.off0", BinaryOp::mul, i64, "%i", source_stride);
    b.binary("%src.off", BinaryOp::add, i64, "%src.off0", source_offset);
    b.getelementptr("%src.slot", Inbounds::yes, i8, "%src", {{i64, "%src.off"}});

    if (nested) {
        const bool source_inline =
            source_fixed && array_layout.inline_fixed_child(source);
        if (source_inline) {
            copy_by_zero_gep(b, "%child.src", Inbounds::yes, "%src.slot");
        } else {
            b.call(runtime_abi::memory::init_check, {{ptr, "%src.slot"}, {i64, "%line"}, {i64, "%column"}});
            b.load("%child.src", ptr, "%src.slot", Align::one);
        }
        b.call("%child.ok", i1, array_cast_validate_name(source_element,target_element),
               {{ptr, "%child.src"}, {i64, "%line"}, {i64, "%column"}});
        b.br("%child.ok", "advance", "fail");
    } else {
        if (!is_numeric(source_element) || !is_numeric(target_element))
            throw std::logic_error("array numeric cast validator leaf must be numeric");
        const auto policy = numeric_conversion_policy(source_element,target_element);
        if (is_tensor_numeric(source_element) && is_tensor_numeric(target_element)) {
            if (policy == NumericConversionPolicy::ExplicitRangeCheck) {
                b.call("%leaf.ok", runtime_abi::numeric::cast_element_fits,
                       {{ptr, "%src.slot"}, {i32, abi::dtype_code(ir::dtype_of(source_element))},
                        {i32, abi::dtype_code(ir::dtype_of(target_element))}, {i64, "%line"}, {i64, "%column"}});
                b.br("%leaf.ok", "advance", "fail");
            } else {
                b.call(runtime_abi::memory::init_check, {{ptr, "%src.slot"}, {i64, "%line"}, {i64, "%column"}});
                b.br("advance");
            }
        } else {
            b.call(runtime_abi::memory::init_check, {{ptr, "%src.slot"}, {i64, "%line"}, {i64, "%column"}});
            if (policy == NumericConversionPolicy::ExplicitRangeCheck &&
                target_element.kind == TypeKind::Nat && is_fixed_integer(source_element)) {
                // A signed fixed-width element fits nat when it is not negative.
                b.load("%nat.src", llvm_type(source_element), "%src.slot", Align::one);
                b.icmp("%leaf.ok", IntPredicate::sge, llvm_type(source_element), "%nat.src", 0);
                b.br("%leaf.ok", "advance", "fail");
            } else if (policy == NumericConversionPolicy::ExplicitRangeCheck) {
                if (!is_bare_integer(source_element) &&
                    source_element.kind != TypeKind::Real) {
                    throw std::logic_error("unsupported fallible exact array cast source");
                }
                int target_kind=0;
                int bits=0;
                if (target_element.kind == TypeKind::Nat) target_kind=7;
                else if (is_bare_integer(target_element)) target_kind=1;
                else if (target_element.kind == TypeKind::Real) target_kind=2;
                else if (is_fixed_integer(target_element)) {
                    target_kind=is_signed_integer(target_element)?3:4;
                    bits=static_cast<int>(integer_width(target_element));
                } else if (target_element.kind == TypeKind::Real32) target_kind=5;
                else if (target_element.kind == TypeKind::Real64) target_kind=6;
                else throw std::logic_error("unsupported fallible exact array cast target");
                if (is_bare_integer(source_element)) {
                    b.load("%exact.src", bare_integer_type, "%src.slot", Align::one);
                    b.call("%leaf.ok", i1, LlvmOperand::global(int_helper::cast_fits),
                           {{bare_integer_type, "%exact.src"}, {i32, target_kind}, {i32, bits}});
                } else {
                    b.load("%exact.src", exact_real_type, "%src.slot", Align::one);
                    b.call("%leaf.ok", i1, LlvmOperand::global(real_helper::cast_fits),
                           {{exact_real_type, "%exact.src"}, {i32, target_kind}, {i32, bits}});
                }
                b.br("%leaf.ok", "advance", "fail");
            } else {
                b.br("advance");
            }
        }
    }
    b.block("advance");
    b.binary("%i.next", BinaryOp::add, i64, "%i", 1);
    b.br("cond");
    b.block("fail");
    b.ret({i1, "false"});
    b.block("done");
    b.ret({i1, "true"});
    b.end_function();
    return text.str();
}

std::string emit_array_cast_helper(const Type& source, const Type& target,
                                   const ArrayLayoutPolicy& array_layout) {
    if (source.kind != TypeKind::Array || target.kind != TypeKind::Array ||
        source.length != target.length) {
        throw std::logic_error("invalid array numeric cast helper types");
    }
    const auto& source_element = *source.first;
    const auto& target_element = *target.first;
    const bool nested = source_element.kind == TypeKind::Array;
    if (nested != (target_element.kind == TypeKind::Array))
        throw std::logic_error("array numeric cast helper nesting mismatch");

    const bool source_fixed = is_fixed_array(source);
    const bool target_fixed = is_fixed_array(target);
    if (source_fixed != target_fixed)
        throw std::logic_error("array numeric cast must preserve static dimensions");

    const auto source_stride = array_element_stride(source,array_layout);
    const auto target_stride = array_element_stride(target,array_layout);
    const auto source_offset = source_fixed ? 0 : abi::array_layout::payload_offset;
    const auto target_offset = target_fixed ? 0 : abi::array_layout::payload_offset;
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(ptr, array_cast_name(source,target))
                 .parameter(ptr, "", "%src").parameter(i64, "", "%line").parameter(i64, "", "%column"));
    b.block("entry");
    b.icmp("%null", IntPredicate::eq, ptr, "%src", "null");
    b.br("%null", "null.ret", "cast.body");
    b.block("null.ret");
    b.ret({ptr, "null"});
    b.block("cast.body");
    if (source_fixed) copy_by_leading_zero_add(b, "%len", i64, source.length);
    else b.load("%len", i64, "%src", Align::one);

    if (target_fixed) {
        const auto storage = fixed_array_storage(target,array_layout);
        const auto bytes = fixed_array_storage_bytes(target,array_layout);
        const auto unit = runtime_storage_bytes(storage.leaf);
        b.call("%dst", runtime_abi::prelude::alloc, {{i64, bytes}});
        b.call(runtime_abi::memory::init_create, {{ptr, "%dst"}, {i64, storage.count}, {i64, unit}, {i64, 0}, {i32, 1}});
    } else {
        b.call("%dst", runtime_abi::prelude::array_alloc, {{i64, "%len"}, {i64, target_stride}, {i32, 1}});
    }

    b.br("cond");
    b.block("cond");
    b.phi("%i", i64, {{0, "cast.body"}, {"%next", "body"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", "%len");
    b.br("%more", "body", "done");
    b.block("body");
    b.binary("%src.off0", BinaryOp::mul, i64, "%i", source_stride);
    b.binary("%src.off", BinaryOp::add, i64, "%src.off0", source_offset);
    b.getelementptr("%src.slot", Inbounds::yes, i8, "%src", {{i64, "%src.off"}});
    b.binary("%dst.off0", BinaryOp::mul, i64, "%i", target_stride);
    b.binary("%dst.off", BinaryOp::add, i64, "%dst.off0", target_offset);
    b.getelementptr("%dst.slot", Inbounds::yes, i8, "%dst", {{i64, "%dst.off"}});

    if (nested) {
        const bool source_inline =
            source_fixed && array_layout.inline_fixed_child(source);
        const bool target_inline =
            target_fixed && array_layout.inline_fixed_child(target);
        if (source_inline) {
            copy_by_zero_gep(b, "%child.src", Inbounds::yes, "%src.slot");
        } else {
            b.call(runtime_abi::memory::init_check, {{ptr, "%src.slot"}, {i64, "%line"}, {i64, "%column"}});
            b.load("%child.src", ptr, "%src.slot", Align::one);
        }
        b.call("%child.dst", ptr, array_cast_name(source_element,target_element),
               {{ptr, "%child.src"}, {i64, "%line"}, {i64, "%column"}});
        if (target_inline) {
            const auto child_bytes =
                fixed_array_storage_bytes(target_element,array_layout);
            b.call(runtime_abi::c_library::memcpy, {{ptr, "%dst.slot"}, {ptr, "%child.dst"}, {i64, child_bytes}});
            b.call(runtime_abi::memory::managed_release, {{ptr, "%child.dst"}, {ptr, "null"}});
        } else {
            b.store({ptr, "%child.dst"}, "%dst.slot", Align::one);
        }
    } else {
        if (!is_numeric(source_element) || !is_numeric(target_element))
            throw std::logic_error("array numeric cast leaf must be numeric");

        if (is_tensor_numeric(source_element) && is_tensor_numeric(target_element)) {
            b.call(runtime_abi::numeric::cast_element,
                   {{ptr, "%dst.slot"}, {ptr, "%src.slot"}, {i32, abi::dtype_code(ir::dtype_of(source_element))},
                    {i32, abi::dtype_code(ir::dtype_of(target_element))}, {i64, "%line"}, {i64, "%column"}});
        } else {
            b.call(runtime_abi::memory::init_check, {{ptr, "%src.slot"}, {i64, "%line"}, {i64, "%column"}});
            const std::string src_value="%exact.cast.src";
            b.load(src_value, llvm_type(source_element), "%src.slot", Align::one);

            if (is_bare_integer(target_element)) {
                if (is_bare_integer(source_element)) {
                    b.call(void_type, LlvmOperand::global(int_helper::retain), {{bare_integer_type, src_value}});
                    b.store({bare_integer_type, src_value}, "%dst.slot", Align::one);
                } else if (source_element.kind == TypeKind::Real) {
                    b.call("%exact.cast.out", bare_integer_type, LlvmOperand::global(real_helper::to_bigint),
                           {{exact_real_type, src_value}, {i64, "%line"}, {i64, "%column"}});
                    b.store({bare_integer_type, "%exact.cast.out"}, "%dst.slot", Align::one);
                } else if (is_fixed_integer(source_element)) {
                    std::string widened=src_value;
                    if (integer_width(source_element)<64) {
                        widened="%exact.cast.widen";
                        b.cast(widened, is_signed_integer(source_element) ? CastOp::sext : CastOp::zext,
                               {llvm_type(source_element), src_value}, i64);
                    }
                    const auto from_integer=is_signed_integer(source_element)?int_helper::from_i64:int_helper::from_u64;
                    b.call("%exact.cast.out", bare_integer_type, LlvmOperand::global(from_integer), {{i64, widened}});
                    b.store({bare_integer_type, "%exact.cast.out"}, "%dst.slot", Align::one);
                } else {
                    throw std::logic_error("unsupported array cast to bigint");
                }
            } else if (target_element.kind == TypeKind::Real) {
                if (source_element.kind == TypeKind::Real) {
                    b.call(void_type, LlvmOperand::global(real_helper::retain), {{exact_real_type, src_value}});
                    b.store({exact_real_type, src_value}, "%dst.slot", Align::one);
                } else if (is_bare_integer(source_element)) {
                    b.call("%exact.cast.out", exact_real_type, LlvmOperand::global(real_helper::from_bigint), {{bare_integer_type, src_value}});
                    b.store({exact_real_type, "%exact.cast.out"}, "%dst.slot", Align::one);
                } else if (is_fixed_integer(source_element)) {
                    std::string widened=src_value;
                    if (integer_width(source_element)<64) {
                        widened="%exact.cast.widen";
                        b.cast(widened, is_signed_integer(source_element) ? CastOp::sext : CastOp::zext,
                               {llvm_type(source_element), src_value}, i64);
                    }
                    const auto from_integer=is_signed_integer(source_element)?real_helper::from_i64:real_helper::from_u64;
                    b.call("%exact.cast.out", exact_real_type, LlvmOperand::global(from_integer), {{i64, widened}});
                    b.store({exact_real_type, "%exact.cast.out"}, "%dst.slot", Align::one);
                } else if (is_fixed_real(source_element)) {
                    std::string widened=src_value;
                    if (source_element.kind==TypeKind::Real32) {
                        widened="%exact.cast.float";
                        b.cast(widened, CastOp::fpext, {float_type, src_value}, double_type);
                    }
                    b.call("%exact.cast.out", exact_real_type, LlvmOperand::global(real_helper::from_float64), {{double_type, widened}});
                    b.store({exact_real_type, "%exact.cast.out"}, "%dst.slot", Align::one);
                } else {
                    throw std::logic_error("unsupported array cast to bigreal");
                }
            } else if (is_fixed_integer(target_element)) {
                std::string integer_source;
                if (source_element.kind == TypeKind::Real) {
                    b.call("%exact.cast.integer", bare_integer_type, LlvmOperand::global(real_helper::to_bigint),
                           {{exact_real_type, src_value}, {i64, "%line"}, {i64, "%column"}});
                    integer_source="%exact.cast.integer";
                } else if (is_bare_integer(source_element)) {
                    integer_source=src_value;
                } else {
                    throw std::logic_error("unsupported exact array integer source");
                }
                const auto raw="%exact.cast.raw";
                const auto to_integer=is_signed_integer(target_element)?int_helper::to_i64:int_helper::to_u64;
                b.call(raw, bare_integer_type, LlvmOperand::global(to_integer),
                       {{bare_integer_type, integer_source}, {i32, integer_width(target_element)}, {i64, "%line"}, {i64, "%column"}});
                if (source_element.kind == TypeKind::Real)
                    b.call(void_type, LlvmOperand::global(int_helper::release), {{bare_integer_type, "%exact.cast.integer"}});
                if (integer_width(target_element)<64)
                    b.cast("%exact.cast.out", CastOp::trunc, {i64, raw}, llvm_type(target_element));
                else
                    copy_by_add_zero(b, "%exact.cast.out", i64, raw);
                b.store({llvm_type(target_element), "%exact.cast.out"}, "%dst.slot", Align::one);
            } else if (is_fixed_real(target_element)) {
                if (!is_bare_integer(source_element) &&
                    source_element.kind != TypeKind::Real)
                    throw std::logic_error("unsupported exact array float source");
                const bool from_real=source_element.kind==TypeKind::Real;
                const bool to_float32=target_element.kind==TypeKind::Real32;
                if (from_real)
                    b.call("%exact.cast.out", to_float32?float_type:double_type,
                           LlvmOperand::global(to_float32?real_helper::to_float32:real_helper::to_float64),
                           {{exact_real_type, src_value}, {i64, "%line"}, {i64, "%column"}});
                else
                    b.call("%exact.cast.out", to_float32?float_type:double_type,
                           LlvmOperand::global(to_float32?int_helper::to_float32:int_helper::to_float64),
                           {{bare_integer_type, src_value}, {i64, "%line"}, {i64, "%column"}});
                b.store({llvm_type(target_element), "%exact.cast.out"}, "%dst.slot", Align::one);
            } else {
                throw std::logic_error("unsupported exact array numeric cast");
            }
        }
    }

    b.binary("%next", BinaryOp::add, i64, "%i", 1);
    b.br("cond");
    b.block("done");
    b.ret({ptr, "%dst"});
    b.end_function();
    return text.str();
}

} // namespace quidra::llvm_backend
