#pragma once

// Typed-IR instructions of the random domain (random generators).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"

#include <cstdint>

namespace quidra::ir {

struct RandomGenerator { ValueId out; ValueId seed; };
struct RandomInt { ValueId out; ValueId generator; ValueId start; ValueId end; std::uint32_t line{}; std::uint32_t column{}; };
struct RandomFloat { ValueId out; ValueId generator; };
struct RandomBool { ValueId out; ValueId generator; };

template <> struct InstructionTraits<RandomGenerator> : InDomain<Domain::random> {};
template <> struct InstructionTraits<RandomInt> : InDomain<Domain::random> {};
template <> struct InstructionTraits<RandomFloat> : InDomain<Domain::random> {};
template <> struct InstructionTraits<RandomBool> : InDomain<Domain::random> {};

} // namespace quidra::ir
