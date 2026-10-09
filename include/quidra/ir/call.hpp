#pragma once

// Typed-IR instructions of the call domain (function references and calls).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::ir {

struct CallArgument { ValueId value; std::optional<ValueId> writable_address; };
struct FunctionRef { ValueId out; std::string function; Type type; };
struct IndirectCall { ValueId out; ValueId callee; std::vector<ValueId> args; std::vector<Type> parameter_types; Type result; std::uint32_t line{}; std::uint32_t column{}; };
struct Call {
    ValueId out;
    std::string callee;
    std::vector<CallArgument> args;
    Type result;
    std::uint32_t line{};
    std::uint32_t column{};
    bool no_normal_return{};
};

template <> struct InstructionTraits<FunctionRef> : InDomain<Domain::call> {};
template <> struct InstructionTraits<IndirectCall> : InDomain<Domain::call> {};
template <> struct InstructionTraits<Call> : InDomain<Domain::call> {};

} // namespace quidra::ir
