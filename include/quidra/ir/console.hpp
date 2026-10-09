#pragma once

// Typed-IR instructions of the console domain (print, input, flush).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

namespace quidra::ir {

struct Flush { ValueId out; Type result_type; };
// print/flush report output failure as an error alternative: the result is
// void | error, and a discarded error fails fast at the statement.
struct Print { ValueId value; Type type; ValueId out; Type result_type; };
struct Input { ValueId out; Type result_type; };

template <> struct InstructionTraits<Flush> : InDomain<Domain::console> {};
template <> struct InstructionTraits<Print> : InDomain<Domain::console> {};
template <> struct InstructionTraits<Input> : InDomain<Domain::console> {};

} // namespace quidra::ir
