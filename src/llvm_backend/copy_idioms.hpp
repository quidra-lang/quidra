#pragma once

// The backend's copy idioms: every value of the typed IR has its own LLVM
// name (%vN), so a constant, or a value already named elsewhere, gets its
// name from an instruction that leaves the operand unchanged. Each idiom is
// one LlvmBuilder operation with a fixed spelling, which is part of the
// output, and is named by that spelling: copy_by_add_zero writes
// add T x, 0, and copy_by_leading_zero_add writes the identity first,
// add T 0, x. Integers, floats and bools are copied with both spellings,
// and the backend keeps both.
//
// Owns nothing; the names are allocated by the caller.

#include "llvm_text/llvm_builder.hpp"

#include <string_view>

namespace quidra::llvm_backend {

// The spelling of the float zero these idioms add.
inline constexpr std::string_view float_zero = "0.000000e+00";

// result = add type 0, value
inline void copy_by_leading_zero_add(llvm_text::LlvmBuilder& builder, std::string_view result,
                                     llvm_text::LlvmType type, llvm_text::LlvmOperand value) {
    builder.binary(result, llvm_text::BinaryOp::add, type, 0, value);
}

// result = fadd type 0.000000e+00, value
inline void copy_by_leading_zero_fadd(llvm_text::LlvmBuilder& builder, std::string_view result,
                                      llvm_text::LlvmType type, llvm_text::LlvmOperand value) {
    builder.binary(result, llvm_text::BinaryOp::fadd, type, float_zero, value);
}

// result = xor i1 false, value
inline void copy_by_leading_false_xor(llvm_text::LlvmBuilder& builder, std::string_view result,
                                      llvm_text::LlvmOperand value) {
    builder.binary(result, llvm_text::BinaryOp::xor_, llvm_text::types::i1, "false", value);
}

// result = add type value, 0
inline void copy_by_add_zero(llvm_text::LlvmBuilder& builder, std::string_view result,
                             llvm_text::LlvmType type, llvm_text::LlvmOperand value) {
    builder.binary(result, llvm_text::BinaryOp::add, type, value, 0);
}

// result = fadd type value, 0.000000e+00
inline void copy_by_fadd_zero(llvm_text::LlvmBuilder& builder, std::string_view result,
                              llvm_text::LlvmType type, llvm_text::LlvmOperand value) {
    builder.binary(result, llvm_text::BinaryOp::fadd, type, value, float_zero);
}

// result = select i1 true, type value, type value
inline void copy_by_select_true(llvm_text::LlvmBuilder& builder, std::string_view result,
                                llvm_text::LlvmType type, llvm_text::LlvmOperand value) {
    builder.select(result, "true", {type, value}, {type, value});
}

// result = getelementptr [inbounds] i8, ptr value, i64 0
inline void copy_by_zero_gep(llvm_text::LlvmBuilder& builder, std::string_view result,
                             llvm_text::Inbounds inbounds, llvm_text::LlvmOperand value) {
    builder.getelementptr(result, inbounds, llvm_text::types::i8, value, {{llvm_text::types::i64, 0}});
}

// result = xor i1 value, false
inline void copy_by_xor_false(llvm_text::LlvmBuilder& builder, std::string_view result,
                              llvm_text::LlvmOperand value) {
    builder.binary(result, llvm_text::BinaryOp::xor_, llvm_text::types::i1, value, "false");
}

} // namespace quidra::llvm_backend
