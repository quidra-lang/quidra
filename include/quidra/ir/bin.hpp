#pragma once

// Typed-IR instructions of the bin domain (byte buffers).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"

#include <cstdint>

namespace quidra::ir {

struct BinAlloc { ValueId out; ValueId length; ValueId fill; };
struct BinLength { ValueId out; ValueId bin; };
struct BinGet { ValueId out; ValueId bin; ValueId index; std::uint32_t line{}; std::uint32_t column{}; bool bounds_proven{}; };
struct BinSet { ValueId bin; ValueId index; ValueId value; std::uint32_t line{}; std::uint32_t column{}; bool bounds_proven{}; };
struct BinSlice { ValueId out; ValueId bin; ValueId start; ValueId end; std::uint32_t line{}; std::uint32_t column{}; };

template <> struct InstructionTraits<BinAlloc> : InDomain<Domain::bin> {};
template <> struct InstructionTraits<BinLength> : InDomain<Domain::bin> {};
template <> struct InstructionTraits<BinGet> : InDomain<Domain::bin> {};
template <> struct InstructionTraits<BinSet> : InDomain<Domain::bin> {};
template <> struct InstructionTraits<BinSlice> : InDomain<Domain::bin> {};

} // namespace quidra::ir
