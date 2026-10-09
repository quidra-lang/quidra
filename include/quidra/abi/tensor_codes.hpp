#pragma once

// The operation codes of the runtime's tensor entry points, which generated
// code passes and the runtime and device code dispatch on.
//
// Owns: the operations of quidra_tensor_binary, quidra_tensor_compare and
// quidra_tensor_unary, the side of a tensor-scalar operation, and the fill
// mode of a new tensor's storage. Each set is a namespace of int constants,
// because the codes cross the C ABI as int parameters and are stored and
// compared as ints on both sides.

namespace quidra::abi {

// quidra_tensor_binary's operation.
namespace tensor_binary_opcode {
inline constexpr int add = 1;
inline constexpr int subtract = 2;
inline constexpr int multiply = 3;
inline constexpr int divide = 4;
inline constexpr int remainder = 5;
inline constexpr int power = 6;
} // namespace tensor_binary_opcode

// quidra_tensor_compare's operation.
namespace tensor_comparison_opcode {
inline constexpr int equal = 1;
inline constexpr int not_equal = 2;
inline constexpr int less = 3;
inline constexpr int less_equal = 4;
inline constexpr int greater = 5;
inline constexpr int greater_equal = 6;
} // namespace tensor_comparison_opcode

// quidra_tensor_unary's operation.
namespace tensor_unary_opcode {
inline constexpr int negate = 1;
} // namespace tensor_unary_opcode

// Which operand of a binary operation or comparison is the scalar: none when
// both are tensors, left in scalar OP tensor, right in tensor OP scalar.
namespace scalar_side {
inline constexpr int none = 0;
inline constexpr int left = 1;
inline constexpr int right = 2;
} // namespace scalar_side

// How a new tensor's storage starts: uninitialized (tracked per element),
// zeros, ones, or write_only: every element is written by its producer before
// the storage is observed (a failed producer releases it), so device storage
// needs no fill. Generated code passes the first three (ir::tensor_fill_mode);
// write_only is the runtime's own.
namespace tensor_fill_mode {
inline constexpr int uninitialized = 0;
inline constexpr int zeros = 1;
inline constexpr int ones = 2;
inline constexpr int write_only = 3;
} // namespace tensor_fill_mode

} // namespace quidra::abi
