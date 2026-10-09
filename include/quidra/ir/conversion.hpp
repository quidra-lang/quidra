#pragma once

// Typed-IR instructions of the conversion domain (numeric casts, parsing, formatting, to-string).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>

namespace quidra::ir {

struct ParseBin { ValueId out; ValueId text; Type result_type; bool success_proven{}; };
struct BinConvert { ValueId out; ValueId value; Type source_type; Type target_type; std::uint32_t line{}; std::uint32_t column{}; };
// inline_proven: a conversion to or from an arbitrary-precision integer
// whose value the lowering proved to be an inline word, and, for a
// conversion to a fixed-width integer, within the target's range
// (check_elision_facts.hpp), so it needs no tag test, promotion or check.
struct NumericConvert {
    ValueId out;
    ValueId value;
    Type source_type;
    Type target_type;
    bool checked_range{};
    std::uint32_t line{};
    std::uint32_t column{};
    bool inline_proven{};
};
struct FallibleNumericConvert { ValueId out; ValueId value; Type source_type; Type target_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct ArrayNumericCast { ValueId out; ValueId array; Type source_type; Type target_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct ParseNumber { ValueId out; ValueId text; Type target_type; Type result_type; };
struct ParseNumberDirect {
    ValueId value_out;
    ValueId ok_out;
    ValueId error_out;
    ValueId text;
    Type target_type;
};
struct ToString { ValueId out; ValueId value; Type source_type; };
struct FormatNumber {
    ValueId out;
    ValueId value;
    Type source_type;
    std::optional<std::uint32_t> integer_width;
    std::optional<std::uint32_t> fractional_digits;
    std::optional<std::uint32_t> significant_digits;
    bool zero{};
};

template <> struct InstructionTraits<ParseBin> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<BinConvert> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<NumericConvert> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<FallibleNumericConvert> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<ArrayNumericCast> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<ParseNumber> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<ParseNumberDirect> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<ToString> : InDomain<Domain::conversion> {};
template <> struct InstructionTraits<FormatNumber> : InDomain<Domain::conversion> {};

} // namespace quidra::ir
