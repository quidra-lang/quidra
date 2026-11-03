#pragma once

// Typed-IR instructions of the bin domain (byte buffers).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/slice.hpp"
#include "quidra/ir/value_id.hpp"

#include <cstdint>

namespace quidra::ir {

struct BinAlloc { ValueId out; ValueId length; ValueId fill; };
struct BinLength { ValueId out; ValueId bin; };
struct BinGet { ValueId out; ValueId bin; ValueId index; SourcePosition position{}; bool bounds_proven{}; };
struct BinSet { ValueId bin; ValueId index; ValueId value; SourcePosition position{}; bool bounds_proven{}; };
struct BinSlice { ValueId out; ValueId bin; ValueId start; ValueId end; SourcePosition position{}; };
// A slice of a bin in any other form than start:<end (no step): the
// bits it selects, in its order.
struct BinSlicePlanned { ValueId out; ValueId bin; SliceBounds bounds; SourcePosition position{}; };

template <> struct InstructionTraits<BinAlloc> : WithOperands<Domain::bin, BinAlloc> {};
template <> struct InstructionTraits<BinLength> : WithOperands<Domain::bin, BinLength> {};
template <> struct InstructionTraits<BinGet> : WithOperands<Domain::bin, BinGet> {};
template <> struct InstructionTraits<BinSet> : WithOperands<Domain::bin, BinSet> {};
template <> struct InstructionTraits<BinSlice> : WithOperands<Domain::bin, BinSlice> {};
template <> struct InstructionTraits<BinSlicePlanned> : WithOperands<Domain::bin, BinSlicePlanned> {};

} // namespace quidra::ir
