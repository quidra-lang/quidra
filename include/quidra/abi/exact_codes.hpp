#pragma once

// The codes of the runtime's exact numbers (big integers and exact reals),
// which generated code passes to the runtime.

namespace quidra::abi {

// The operation of quidra_bigint_binary and quidra_bigreal_binary. Exact
// reals take add to divide; remainder is an integer operation.
namespace exact_binary_opcode {
inline constexpr int add = 1;
inline constexpr int subtract = 2;
inline constexpr int multiply = 3;
inline constexpr int divide = 4;
inline constexpr int remainder = 5;
} // namespace exact_binary_opcode

// The significant digits of an exact real's decimal text when no precision
// is given (quidra_bigreal_text with 0 digits, printing and string
// conversion).
inline constexpr int exact_real_display_digits = 34;

} // namespace quidra::abi
