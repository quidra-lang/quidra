#pragma once

// Typed-IR instructions of the constant domain (literal values).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <string>

namespace quidra::ir {

struct ConstantInt { ValueId out; std::string value; Type type; };
// `spelling` is the decimal text of the source literal the value comes from
// (a real literal, or an integer literal in a real context), or empty for a
// value the compiler makes. `value` is the literal's binary64 value.
struct ConstantFloat { ValueId out; double value; Type type; std::string spelling; };
struct ConstantExact { ValueId out; std::string spelling; Type type; };
struct ConstantBool { ValueId out; bool value; };
struct ConstantString { ValueId out; std::string value; };

template <> struct InstructionTraits<ConstantInt> : InDomain<Domain::constant> {};
template <> struct InstructionTraits<ConstantFloat> : InDomain<Domain::constant> {};
template <> struct InstructionTraits<ConstantExact> : InDomain<Domain::constant> {};
template <> struct InstructionTraits<ConstantBool> : InDomain<Domain::constant> {};
template <> struct InstructionTraits<ConstantString> : InDomain<Domain::constant> {};

} // namespace quidra::ir
