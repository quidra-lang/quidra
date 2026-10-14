#pragma once

// Typed-IR instructions of the random domain (random generators).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"

#include <cstdint>

namespace quidra::ir {

struct RandomGenerator { ValueId out; ValueId seed; };
struct RandomInt { ValueId out; ValueId generator; ValueId start; ValueId end; SourcePosition position{}; };
struct RandomFloat { ValueId out; ValueId generator; };
struct RandomBool { ValueId out; ValueId generator; };

template <> struct InstructionTraits<RandomGenerator> : WithOperands<Domain::random, RandomGenerator> {};
template <> struct InstructionTraits<RandomInt> : WithOperands<Domain::random, RandomInt> {};
template <> struct InstructionTraits<RandomFloat> : WithOperands<Domain::random, RandomFloat> {};
template <> struct InstructionTraits<RandomBool> : WithOperands<Domain::random, RandomBool> {};

} // namespace quidra::ir
