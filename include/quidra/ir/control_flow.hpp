#pragma once

// Typed-IR instructions of the control_flow domain (block terminators).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <string>

namespace quidra::ir {

struct Exit { ValueId status; };
struct FailError { ValueId error; SourcePosition position{}; };
struct Return { ValueId value; Type type; };
struct ReturnVoid {};
struct Jump { std::string target; };
struct Branch { ValueId condition; std::string if_true; std::string if_false; };

template <> struct InstructionTraits<Exit> : WithOperands<Domain::control_flow, Exit> {};
template <> struct InstructionTraits<FailError> : WithOperands<Domain::control_flow, FailError> {};
template <> struct InstructionTraits<Return> : WithOperands<Domain::control_flow, Return> {};
template <> struct InstructionTraits<ReturnVoid> : WithOperands<Domain::control_flow, ReturnVoid> {};
template <> struct InstructionTraits<Jump> : WithOperands<Domain::control_flow, Jump> {};
template <> struct InstructionTraits<Branch> : WithOperands<Domain::control_flow, Branch> {};

} // namespace quidra::ir
