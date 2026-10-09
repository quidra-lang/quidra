#pragma once

// Type lowering: how a Quidra Type is spelled in LLVM textual IR.
//
// Owns the LLVM type of each Quidra type (an LlvmType of the LLVM text
// layer), the identifier that names a type inside helper symbols, and the
// literal of a float constant. These read quidra::Type, so they belong to
// the backend, not to the LLVM text layer. llvm_type is inline because the
// emitter calls it for nearly every instruction it writes; type_identifier
// is inline because the emitter reaches it through the layout check of
// nested fixed arrays (ArrayLayoutPolicy::inline_fixed_child) and through
// every helper symbol name (clone, drop, equality, array cast).

#include "quidra/types.hpp"
#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/small_rational.hpp"
#include "llvm_text/escape.hpp"
#include "llvm_text/llvm_type.hpp"

#include <string>

namespace quidra::llvm_backend {

// The LLVM type of a value of type t (i64, double, ptr, void, ...; a `real`
// is { i64, i64 }, an arbitrary-precision integer one i64 word).
inline llvm_text::LlvmType llvm_type(const Type& t) {
    using llvm_text::LlvmType;
    switch(t.kind){
        case TypeKind::Int64: case TypeKind::Nat64: return LlvmType::Scalar::i64;
        case TypeKind::Int8: case TypeKind::Nat8: return LlvmType::Scalar::i8;
        case TypeKind::Int16: case TypeKind::Nat16: return LlvmType::Scalar::i16;
        case TypeKind::Int32: case TypeKind::Nat32: return LlvmType::Scalar::i32;
        case TypeKind::Real64: return LlvmType::Scalar::double_type;
        case TypeKind::Real32: return LlvmType::Scalar::float_type;
        case TypeKind::Bool: return LlvmType::Scalar::i1;
        case TypeKind::Real: return exact_real_type;
        case TypeKind::Int: case TypeKind::Nat: return bare_integer_type;
        case TypeKind::String: case TypeKind::Bin: case TypeKind::Error:
        case TypeKind::Array: case TypeKind::Tensor:
        case TypeKind::Union: case TypeKind::Class:
        case TypeKind::Address: case TypeKind::Function: return LlvmType::Scalar::ptr;
        case TypeKind::None: case TypeKind::Void: case TypeKind::Never: return LlvmType::Scalar::void_type;
        case TypeKind::Auto: case TypeKind::Range: case TypeKind::Invalid: return LlvmType::Scalar::void_type;
    }
    return LlvmType::Scalar::void_type;
}

// type_name(t) as an identifier: helper symbols (@quidra_clone_<id>, ...) and
// the boxed fixed-array edges are keyed by it.
inline std::string type_identifier(const Type& t){return llvm_text::sanitize_identifier(type_name(t));}
// The LLVM literal of a float constant of type `type`: the bits of the double
// in hex, after rounding through float when `type` is float32.
std::string float_literal(double value, const Type& type);

} // namespace quidra::llvm_backend
