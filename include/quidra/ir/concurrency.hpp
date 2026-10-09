#pragma once

// Typed-IR instructions of the concurrency domain (tasks and atomic counters).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>

namespace quidra::ir {

struct TaskAll { ValueId out{}; ValueId operations{}; ValueId shared{}; Type result_type; Type shared_type; std::uint32_t line{}; std::uint32_t column{}; };
struct AtomicCounterCreate { ValueId out; ValueId initial; };
struct AtomicCounterAdd { ValueId out; ValueId counter; ValueId delta; std::uint32_t line{}; std::uint32_t column{}; };
struct AtomicCounterLoad { ValueId out; ValueId counter; };

template <> struct InstructionTraits<TaskAll> : InDomain<Domain::concurrency> {};
template <> struct InstructionTraits<AtomicCounterCreate> : InDomain<Domain::concurrency> {};
template <> struct InstructionTraits<AtomicCounterAdd> : InDomain<Domain::concurrency> {};
template <> struct InstructionTraits<AtomicCounterLoad> : InDomain<Domain::concurrency> {};

} // namespace quidra::ir
