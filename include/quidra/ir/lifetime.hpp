#pragma once

// Typed-IR instructions of the lifetime domain (clone, retain, release, pin,
// unpin).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

namespace quidra::ir {

struct Clone { ValueId out; ValueId value; Type type; };
struct Retain { ValueId out; ValueId value; Type type; };
struct Release { ValueId value; Type type; };
// Keeps the managed allocation that holds `value` (the allocation itself or
// the one containing it) until the matching Unpin: it is not moved, and a
// release of its last owner defers its finalization to the unpin.
struct Pin { ValueId value; };
struct Unpin { ValueId value; };

template <> struct InstructionTraits<Clone> : WithOperands<Domain::lifetime, Clone> {};
template <> struct InstructionTraits<Retain> : WithOperands<Domain::lifetime, Retain> {};
template <> struct InstructionTraits<Release> : WithOperands<Domain::lifetime, Release> {};
template <> struct InstructionTraits<Pin> : WithOperands<Domain::lifetime, Pin> {};
template <> struct InstructionTraits<Unpin> : WithOperands<Domain::lifetime, Unpin> {};

} // namespace quidra::ir
