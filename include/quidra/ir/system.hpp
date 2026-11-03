#pragma once

// Typed-IR instructions of the system domain (command line, environment, clock, processes).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
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
struct TimeSleep { ValueId duration; SourcePosition position{}; };
struct ProcessRun { ValueId out; ValueId program; ValueId args; };
struct ProcessShell { ValueId out; ValueId command; };

template <> struct InstructionTraits<CliArgument> : WithOperands<Domain::system, CliArgument> {};
template <> struct InstructionTraits<CliArgumentOptional> : WithOperands<Domain::system, CliArgumentOptional> {};
template <> struct InstructionTraits<CliOption> : WithOperands<Domain::system, CliOption> {};
template <> struct InstructionTraits<CliFlag> : WithOperands<Domain::system, CliFlag> {};
template <> struct InstructionTraits<CliFinish> : WithOperands<Domain::system, CliFinish> {};
template <> struct InstructionTraits<EnvironmentGet> : WithOperands<Domain::system, EnvironmentGet> {};
template <> struct InstructionTraits<EnvironmentHas> : WithOperands<Domain::system, EnvironmentHas> {};
template <> struct InstructionTraits<TimeNow> : WithOperands<Domain::system, TimeNow> {};
template <> struct InstructionTraits<TimeSince> : WithOperands<Domain::system, TimeSince> {};
template <> struct InstructionTraits<TimeSeconds> : WithOperands<Domain::system, TimeSeconds> {};
template <> struct InstructionTraits<TimeSleep> : WithOperands<Domain::system, TimeSleep> {};
template <> struct InstructionTraits<ProcessRun> : WithOperands<Domain::system, ProcessRun> {};
template <> struct InstructionTraits<ProcessShell> : WithOperands<Domain::system, ProcessShell> {};

} // namespace quidra::ir
