#pragma once

// Typed-IR instructions of the http domain (HTTP requests).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

namespace quidra::ir {

struct HttpGet { ValueId out; ValueId url; Type result_type; };
struct HttpHeader { ValueId out; ValueId response; ValueId name; Type result_type; };

template <> struct InstructionTraits<HttpGet> : InDomain<Domain::http> {};
template <> struct InstructionTraits<HttpHeader> : InDomain<Domain::http> {};

} // namespace quidra::ir
