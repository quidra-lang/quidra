#pragma once

// Typed-IR instructions of the system domain (command line, environment, clock, processes).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>

namespace quidra::ir {

struct CliArgument { ValueId out; ValueId name; ValueId index; Type type; };
struct CliArgumentOptional { ValueId out; ValueId name; ValueId index; ValueId default_value; Type type; };
struct CliOption { ValueId out; ValueId name; ValueId default_value; Type type; };
struct CliFlag { ValueId out; ValueId name; };
struct CliFinish {};
struct EnvironmentGet { ValueId out; ValueId name; Type result_type; };
struct EnvironmentHas { ValueId out; ValueId name; };
struct TimeNow { ValueId out; ValueId sync; };
struct TimeSince { ValueId out; ValueId start; ValueId sync; };
struct TimeSeconds { ValueId out; ValueId seconds; };
struct TimeSleep { ValueId duration; std::uint32_t line{}; std::uint32_t column{}; };
struct ProcessRun { ValueId out; ValueId program; ValueId args; };
struct ProcessShell { ValueId out; ValueId command; };

template <> struct InstructionTraits<CliArgument> : InDomain<Domain::system> {};
template <> struct InstructionTraits<CliArgumentOptional> : InDomain<Domain::system> {};
template <> struct InstructionTraits<CliOption> : InDomain<Domain::system> {};
template <> struct InstructionTraits<CliFlag> : InDomain<Domain::system> {};
template <> struct InstructionTraits<CliFinish> : InDomain<Domain::system> {};
template <> struct InstructionTraits<EnvironmentGet> : InDomain<Domain::system> {};
template <> struct InstructionTraits<EnvironmentHas> : InDomain<Domain::system> {};
template <> struct InstructionTraits<TimeNow> : InDomain<Domain::system> {};
template <> struct InstructionTraits<TimeSince> : InDomain<Domain::system> {};
template <> struct InstructionTraits<TimeSeconds> : InDomain<Domain::system> {};
template <> struct InstructionTraits<TimeSleep> : InDomain<Domain::system> {};
template <> struct InstructionTraits<ProcessRun> : InDomain<Domain::system> {};
template <> struct InstructionTraits<ProcessShell> : InDomain<Domain::system> {};

} // namespace quidra::ir
