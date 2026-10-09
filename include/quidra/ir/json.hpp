#pragma once

// Typed-IR instructions of the json domain (JSON values).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

namespace quidra::ir {

struct JsonParse { ValueId out; ValueId text; Type result_type; };
struct JsonKind { ValueId out; ValueId value; };
struct JsonSize { ValueId out; ValueId value; Type result_type; };
struct JsonGet { ValueId out; ValueId value; ValueId key; Type result_type; };
struct JsonAt { ValueId out; ValueId value; ValueId index; Type result_type; };
struct JsonText { ValueId out; ValueId value; Type result_type; };
struct JsonInteger { ValueId out; ValueId value; Type result_type; };
struct JsonNumber { ValueId out; ValueId value; Type result_type; };
struct JsonBigInt { ValueId out; ValueId value; Type result_type; };
struct JsonBigReal { ValueId out; ValueId value; Type result_type; };
struct JsonBoolean { ValueId out; ValueId value; Type result_type; };
struct JsonEncode { ValueId out; ValueId value; };
struct JsonEqual { ValueId out; ValueId left; ValueId right; };

template <> struct InstructionTraits<JsonParse> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonKind> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonSize> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonGet> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonAt> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonText> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonInteger> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonNumber> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonBigInt> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonBigReal> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonBoolean> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonEncode> : InDomain<Domain::json> {};
template <> struct InstructionTraits<JsonEqual> : InDomain<Domain::json> {};

} // namespace quidra::ir
