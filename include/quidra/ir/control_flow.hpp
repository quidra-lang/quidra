#pragma once

// Typed-IR instructions of the control_flow domain (block terminators).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <string>

namespace quidra::ir {

struct Exit { ValueId status; };
struct FailError { ValueId error; std::uint32_t line{}; std::uint32_t column{}; };
struct Return { ValueId value; Type type; };
struct ReturnVoid {};
struct Jump { std::string target; };
struct Branch { ValueId condition; std::string if_true; std::string if_false; };

template <> struct InstructionTraits<Exit> : InDomain<Domain::control_flow> {};
template <> struct InstructionTraits<FailError> : InDomain<Domain::control_flow> {};
template <> struct InstructionTraits<Return> : InDomain<Domain::control_flow> {};
template <> struct InstructionTraits<ReturnVoid> : InDomain<Domain::control_flow> {};
template <> struct InstructionTraits<Jump> : InDomain<Domain::control_flow> {};
template <> struct InstructionTraits<Branch> : InDomain<Domain::control_flow> {};

} // namespace quidra::ir
