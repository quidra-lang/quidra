#pragma once

// Typed-IR instructions of the http domain (HTTP requests).
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

struct HttpGet { ValueId out; ValueId url; Type result_type; SourcePosition position{}; };
struct HttpHeader { ValueId out; ValueId response; ValueId name; Type result_type; };

template <> struct InstructionTraits<HttpGet> : WithOperands<Domain::http, HttpGet> {};
template <> struct InstructionTraits<HttpHeader> : WithOperands<Domain::http, HttpHeader> {};

} // namespace quidra::ir
