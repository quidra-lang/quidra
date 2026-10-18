#pragma once

// HalfPrecision: the 16-bit reals in generated code.
//
// real16 is LLVM's `half`: its arithmetic, comparisons and conversions to
// and from float are LLVM's; binary64-to-real16 uses an explicit runtime
// conversion for platform independence and direct nearest-even rounding.
// (On targets without half arithmetic LLVM computes in float and rounds,
// correctly since 24 >= 2 * 11 + 2 bits.) real16b is held
// as an i16 (its bfloat16 bits), so generated code does not depend on the
// `bfloat` support of the installed LLVM: its arithmetic widens both
// operands to float exactly (@quidra.half.bf16.widen), computes in float and
// rounds once to bfloat16, to nearest with ties to even
// (@quidra.half.bf16.narrow), which is correctly rounded too
// (24 >= 2 * 8 + 2). Conversions that need a single correct rounding from a
// wider format (real64 to either half format and 64-bit integers to
// real16b), the text of both
// types (the shortest decimal that reads back to the value, per format) and
// their parsing are runtime entries (quidra_half_*, quidra_real16b_*).
//
// Owns: the helpers and the support block a module naming any of them
// carries, which also declares those runtime entries.

#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "llvm_text/llvm_callee.hpp"
#include "quidra/types.hpp"

#include <span>
#include <string>
#include <string_view>

namespace quidra::llvm_backend {

// The names of the helpers, as call targets (without the leading '@').
namespace half_helper {
// float @quidra.half.bf16.widen(i16): the exact float of bfloat16 bits.
inline constexpr std::string_view bf16_widen = "quidra.half.bf16.widen";
// i16 @quidra.half.bf16.narrow(float): the bfloat16 nearest to a float, ties
// to even; a NaN stays a NaN.
inline constexpr std::string_view bf16_narrow = "quidra.half.bf16.narrow";
} // namespace half_helper

// The format codes the text and parse entries take.
inline constexpr int half_format_real16 = 0;
inline constexpr int half_format_real16b = 1;

inline bool is_half_real(const Type& type) {
    return type.kind == TypeKind::Real16 || type.kind == TypeKind::Real16b;
}

inline int half_format_code(const Type& type) {
    return type.kind == TypeKind::Real16b ? half_format_real16b : half_format_real16;
}

// The 16 bits of `value`, a real16 or real16b of `type`, zero-extended to the
// i32 the runtime entries take them as (a 32-bit C parameter, so no
// platform's rule for extending narrow arguments applies).
// Portable, directly rounded binary64 -> binary16 conversion. The runtime
// returns bits, so generated code does not require a platform half soft-float
// helper (for example __truncdfhf2 on Windows).
std::string real16_from_float64(llvm_text::LlvmBuilder& builder, TemporaryNames& names,
                                const std::string& value);

std::string half_bits(llvm_text::LlvmBuilder& builder, TemporaryNames& names, const Type& type,
                      const std::string& value);

// The prefixes a module's bodies contain when they name a helper or an entry.
inline constexpr std::string_view half_helper_prefix = "@quidra.half.";
inline constexpr std::string_view half_entry_prefix = "@quidra_half_";
inline constexpr std::string_view real16b_entry_prefix = "@quidra_real16b_";

// The declarations of the support block.
std::span<const llvm_text::LlvmCallee* const> half_support_declarations();

// The support block's text.
std::string_view half_support_text();

} // namespace quidra::llvm_backend
