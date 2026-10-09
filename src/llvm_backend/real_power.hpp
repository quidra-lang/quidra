#pragma once

// FixedRealPower: `^` on fixed-width reals in generated code.
//
// Owns: the helpers @quidra.power.real64 and @quidra.power.real32 and their
// support block, which a module using them carries. Each rejects the powers
// that have no value before it calls the C library's pow: a zero base with a
// zero or negative exponent, and a negative base with an exponent that is
// not an integer. Those stop the program with POWER_DOMAIN at the source
// line and column; every other power is the library's.

#include <string_view>

namespace quidra::llvm_backend {

// The names of the helpers, as call targets (without the leading '@').
namespace power_helper {
inline constexpr std::string_view real64 = "quidra.power.real64";
inline constexpr std::string_view real32 = "quidra.power.real32";
} // namespace power_helper

// The prefix every helper call names; a module whose bodies contain it needs
// the support block.
inline constexpr std::string_view real_power_helper_prefix = "@quidra.power.";

// The support block: its failure texts and the helpers. The C library's pow
// and powf and quidra_fail_at are declared by the runtime prelude.
std::string_view real_power_support_text();

} // namespace quidra::llvm_backend
