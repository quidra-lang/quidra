#pragma once

// Typed-IR instructions of the json domain (JSON values).
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

struct JsonParse { ValueId out; ValueId text; Type result_type; SourcePosition position{}; };
struct JsonKind { ValueId out; ValueId value; };
struct JsonSize { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonGet { ValueId out; ValueId value; ValueId key; Type result_type; SourcePosition position{}; };
struct JsonAt { ValueId out; ValueId value; ValueId index; Type result_type; SourcePosition position{}; };
struct JsonText { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonInteger { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonNumber { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonBigInt { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonBigReal { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonBoolean { ValueId out; ValueId value; Type result_type; SourcePosition position{}; };
struct JsonEncode { ValueId out; ValueId value; };
struct JsonEqual { ValueId out; ValueId left; ValueId right; };

template <> struct InstructionTraits<JsonParse> : WithOperands<Domain::json, JsonParse> {};
template <> struct InstructionTraits<JsonKind> : WithOperands<Domain::json, JsonKind> {};
template <> struct InstructionTraits<JsonSize> : WithOperands<Domain::json, JsonSize> {};
template <> struct InstructionTraits<JsonGet> : WithOperands<Domain::json, JsonGet> {};
template <> struct InstructionTraits<JsonAt> : WithOperands<Domain::json, JsonAt> {};
template <> struct InstructionTraits<JsonText> : WithOperands<Domain::json, JsonText> {};
template <> struct InstructionTraits<JsonInteger> : WithOperands<Domain::json, JsonInteger> {};
template <> struct InstructionTraits<JsonNumber> : WithOperands<Domain::json, JsonNumber> {};
template <> struct InstructionTraits<JsonBigInt> : WithOperands<Domain::json, JsonBigInt> {};
template <> struct InstructionTraits<JsonBigReal> : WithOperands<Domain::json, JsonBigReal> {};
template <> struct InstructionTraits<JsonBoolean> : WithOperands<Domain::json, JsonBoolean> {};
template <> struct InstructionTraits<JsonEncode> : WithOperands<Domain::json, JsonEncode> {};
template <> struct InstructionTraits<JsonEqual> : WithOperands<Domain::json, JsonEqual> {};

} // namespace quidra::ir
