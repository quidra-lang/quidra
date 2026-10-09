#pragma once

// The C ABI of foreign functions: the LLVM parameter and return attributes
// with which a user extern (and a call to one) passes values to C, and with
// which the C entry point of an exported function (export "C") receives
// them.
//
// Owns: sign/zero extension of narrow integers and bool, and the pointer
// attributes of string, bin and tensor arguments. User externs and exports
// only; the runtime's own entry points are declared by the runtime prelude.

#include "quidra/types.hpp"

#include <string_view>

namespace quidra::llvm_backend {

// The attribute text written before the return type ("signext ", "zeroext ")
// or "" when none applies. The texts are literals (static storage).
std::string_view c_abi_return_attribute(const Type& type);
// The attribute text that follows a parameter's type: extension of narrow
// integers and bool, and " nocapture nonnull readonly" for strings; bins and
// tensors drop readonly when the C function may write the buffer
// (readonly_buffer=false, a non-const extern parameter).
std::string_view c_abi_parameter_attribute(const Type& type, bool readonly_buffer=true);

// An exported function's C entry point: its result extends 8- and 16-bit
// integers as the callee (signext/zeroext, which Clang's x86-64 and Darwin
// conventions require and which is harmless elsewhere); its parameters carry
// no extension attribute, because the callee extends narrow arguments itself
// and AAPCS64 Linux callers leave their upper bits unspecified.
std::string_view export_return_attribute(const Type& type);
std::string_view export_parameter_attribute(const Type& type);

} // namespace quidra::llvm_backend
