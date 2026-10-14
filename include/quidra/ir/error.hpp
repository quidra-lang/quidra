#pragma once

// Typed-IR instructions of the error domain (error values: their
// construction and the reads of their parts).
//
// An `error` is a pointer to an abi::ErrorRecord (quidra/abi/error_record.hpp):
// a code, the details its schema defines, a cause, an origin and the hops it
// propagated through; its message is the rendering of the code and the
// details.
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::ir {

// One detail of ErrorMake, in the order of its schema: a value of the
// program (a string for a text detail, an integer for an integer detail),
// or a constant (a name detail's spelling, or a constant text), or absent
// (an optional detail the error lacks).
struct ErrorDetailOperand {
    std::optional<ValueId> value;
    std::string constant;
    bool absent{};
};

// Makes an error: `schema` is the abi::ErrorValueCode of a Core code, 0 for
// a user or package error, whose one detail is its text (and `code` its
// code, empty for ERROR); `cause` an error the new one holds; the position
// is where it is made. The result is owned.
struct ErrorMake {
    ValueId out;
    std::uint32_t schema{};
    std::string code;
    std::vector<ErrorDetailOperand> details;
    std::optional<ValueId> cause;
    SourcePosition position{};
};

// The message of an error (e.message, e.string(), an interpolated error): a
// string the result owns.
struct ErrorMessage { ValueId out; ValueId error; };
// The code of an error (e.code): "ERROR" for a user error without a code.
struct ErrorCode { ValueId out; ValueId error; };
// The cause of an error (e.cause): an owned `error | none`.
struct ErrorCause { ValueId out; ValueId error; Type result_type; };
// A detail of an error (e.path, e.reason, ...): `detail` is the detail's
// index in abi::error_detail_names; an owned `string | none`, or
// `nat | none` for the integer detail, none when the error's code has no
// such detail or the error lacks it.
struct ErrorDetail { ValueId out; ValueId error; std::uint32_t detail{}; Type result_type; };

// A hop: the error of the `T | error` cell `container`, which a call
// returned and the function owns, left that call on the error path (`try`,
// a fail-fast stop, or a binding error case of `match`). The hop takes the
// error out of the cell (its payload slot set to null, the cell still
// released as before) and records the call site on it: `name` the display
// name of the called function, `kind` an abi::ErrorHopKind, the position the
// call's. A call through an `func` value also gives the value it called
// (`callee`, 0 for a direct call), so that the report names the function
// actually called. The result is the error, owned.
struct ErrorHop {
    ValueId out;
    ValueId container;
    Type container_type;
    std::string name;
    std::uint32_t kind{};
    SourcePosition position{};
    ValueId callee{};
};

template <> struct InstructionTraits<ErrorMake> : WithOperands<Domain::error, ErrorMake> {};
template <> struct InstructionTraits<ErrorHop> : WithOperands<Domain::error, ErrorHop> {};
template <> struct InstructionTraits<ErrorMessage> : WithOperands<Domain::error, ErrorMessage> {};
template <> struct InstructionTraits<ErrorCode> : WithOperands<Domain::error, ErrorCode> {};
template <> struct InstructionTraits<ErrorCause> : WithOperands<Domain::error, ErrorCause> {};
template <> struct InstructionTraits<ErrorDetail> : WithOperands<Domain::error, ErrorDetail> {};

} // namespace quidra::ir
