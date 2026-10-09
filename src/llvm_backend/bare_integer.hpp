#pragma once

// BareIntegerArithmetic: the arbitrary-precision integer type in generated
// code.
//
// Owns: the LLVM type of an arbitrary-precision integer, one i64 word
// (abi::bare_integer_layout: an inline value shifted left by one, or the
// address of a managed big integer with the low bit set), and the support
// block that a module using such words carries: the declarations of the
// word entry points of the runtime (quidra_int_*) and the helpers
// @quidra.int.* written in LLVM IR. The helpers take the inline fast paths
// themselves: addition and subtraction on the words with an overflow check
// (the words are twice the values, so a 64-bit overflow is exactly a
// result outside the inline range), multiplication of one untagged operand
// by the other word, division and remainder by a nonzero inline divisor,
// negation, comparisons on the words, and conversions from and to
// fixed-width integers and reals. Every other case calls the runtime, which
// keeps the values, texts and failures of the big-integer operations.
//
// Copying a word retains its box and dropping it releases the box; both
// test the low bit first, so inline words cost one test.

#include "llvm_text/llvm_callee.hpp"
#include "llvm_text/llvm_type.hpp"

#include <span>
#include <string>
#include <string_view>

namespace quidra::llvm_backend {

// The LLVM type of an arbitrary-precision integer value.
inline constexpr llvm_text::LlvmType bare_integer_type = llvm_text::types::i64;

// The names of the helpers, as call targets (without the leading '@').
namespace int_helper {
inline constexpr std::string_view retain = "quidra.int.retain";
inline constexpr std::string_view release = "quidra.int.release";
inline constexpr std::string_view add = "quidra.int.add";
inline constexpr std::string_view sub = "quidra.int.sub";
inline constexpr std::string_view mul = "quidra.int.mul";
inline constexpr std::string_view div = "quidra.int.div";
inline constexpr std::string_view rem = "quidra.int.rem";
inline constexpr std::string_view neg = "quidra.int.neg";
inline constexpr std::string_view pow = "quidra.int.pow";
inline constexpr std::string_view compare = "quidra.int.compare";
inline constexpr std::string_view eq = "quidra.int.eq";
inline constexpr std::string_view ne = "quidra.int.ne";
inline constexpr std::string_view lt = "quidra.int.lt";
inline constexpr std::string_view le = "quidra.int.le";
inline constexpr std::string_view gt = "quidra.int.gt";
inline constexpr std::string_view ge = "quidra.int.ge";
inline constexpr std::string_view text = "quidra.int.text";
inline constexpr std::string_view print = "quidra.int.print";
inline constexpr std::string_view format = "quidra.int.format";
// A part of a typed string build (abi::string_build_part_kind): a signed
// integer part for an inline word, the text of the value for a boxed one,
// which it returns for the caller to release (else null).
inline constexpr std::string_view build_part = "quidra.int.build_part";
inline constexpr std::string_view parse = "quidra.int.parse";
inline constexpr std::string_view literal = "quidra.int.literal";
inline constexpr std::string_view adopt = "quidra.int.adopt";
inline constexpr std::string_view from_i64 = "quidra.int.from_i64";
inline constexpr std::string_view from_u64 = "quidra.int.from_u64";
inline constexpr std::string_view to_i64 = "quidra.int.to_i64";
inline constexpr std::string_view to_u64 = "quidra.int.to_u64";
inline constexpr std::string_view try_i64 = "quidra.int.try_i64";
inline constexpr std::string_view try_u64 = "quidra.int.try_u64";
inline constexpr std::string_view to_float64 = "quidra.int.to_float64";
inline constexpr std::string_view to_float32 = "quidra.int.to_float32";
inline constexpr std::string_view try_float64 = "quidra.int.try_float64";
inline constexpr std::string_view try_float32 = "quidra.int.try_float32";
inline constexpr std::string_view cast_fits = "quidra.int.cast_fits";
// nat subtraction (a negative result fails), and the checked conversions
// to nat from int and from a signed fixed-width integer.
inline constexpr std::string_view nat_sub = "quidra.nat.sub";
inline constexpr std::string_view nat_from_int = "quidra.nat.from_int";
inline constexpr std::string_view nat_from_i64 = "quidra.nat.from_i64";
// task.all of operations whose results are arbitrary-precision integers.
inline constexpr std::string_view task_all = "quidra.int.task_all";
} // namespace int_helper

// The prefixes every helper call names; a module whose bodies or helpers
// contain one needs the support block.
inline constexpr std::string_view bare_integer_helper_prefix = "@quidra.int.";
inline constexpr std::string_view natural_helper_prefix = "@quidra.nat.";

// The support block: the declarations of the word entry points, then the
// helpers. The managed-storage entry points it calls are declared by the
// runtime prelude.
std::string_view bare_integer_support_text();

// The runtime entry points the support block declares.
std::span<const llvm_text::LlvmCallee* const> bare_integer_support_declarations();

// The inline word of an integer literal's decimal spelling (an optional
// sign, then digits), when its value is within the inline range.
bool bare_integer_inline_literal(std::string_view spelling, long long& word);

} // namespace quidra::llvm_backend
