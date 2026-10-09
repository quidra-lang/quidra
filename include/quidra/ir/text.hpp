#pragma once

// Typed-IR instructions of the text domain (strings).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <vector>

namespace quidra::ir {

struct StringIndex { ValueId out; ValueId text; ValueId index; std::uint32_t line{}; std::uint32_t column{}; };
struct StringIndexAsciiCompare { ValueId out; ValueId text; ValueId index; unsigned char byte{}; bool negate{}; std::uint32_t line{}; std::uint32_t column{}; };
struct StringAsciiCountPrefix {
    ValueId out;
    ValueId text;
    ValueId count;
    ValueId initial;
    unsigned char byte{};
    bool negate{};
    std::uint32_t index_line{};
    std::uint32_t index_column{};
    std::uint32_t overflow_line{};
    std::uint32_t overflow_column{};
};
struct StringLength { ValueId out; ValueId text; };
struct StringEmpty { ValueId out; ValueId text; bool negate{}; };
struct StringContains { ValueId out; ValueId text; ValueId needle; };
struct StringStartsWith { ValueId out; ValueId text; ValueId prefix; };
struct StringEndsWith { ValueId out; ValueId text; ValueId suffix; };
struct StringFind { ValueId out; ValueId text; ValueId needle; Type result_type; };
struct StringSlice { ValueId out; ValueId text; ValueId start; ValueId end; std::uint32_t line{}; std::uint32_t column{}; };
struct StringTrim { ValueId out; ValueId text; };
struct StringSplit { ValueId out; ValueId text; ValueId separator; };
struct StringSplitIterBegin { ValueId out; ValueId text; ValueId separator; bool move_source{}; };
struct StringSplitIterNext { ValueId text; ValueId has_value; ValueId cursor; };
struct StringSplitIterEnd { ValueId cursor; };
struct StringParseTwoSigned { ValueId left; ValueId right; ValueId ok; ValueId text; unsigned char separator{}; };
struct StringUtf8 { ValueId out; ValueId text; };
struct StringFromUtf8 { ValueId out; ValueId bin; Type result_type; };
struct StringFromUtf8ArrayDirect { ValueId text; ValueId ok; ValueId error; ValueId array; };
struct StringCodepoints { ValueId out; ValueId text; };
struct StringJoin { ValueId out; ValueId values; ValueId separator; std::uint32_t line{}; std::uint32_t column{}; };
struct StringConcat { ValueId out; std::vector<ValueId> values; };
struct StringBuildPart { ValueId value; Type type; bool single_byte_ascii{}; };
struct StringBuild { ValueId out; std::vector<StringBuildPart> parts; ValueId separator; };
struct StringBuildAppendMove { ValueId out; ValueId added_length; ValueId text; std::vector<StringBuildPart> parts; ValueId separator; };
struct StringCanAppendMove { ValueId out; ValueId text; };
struct StringAppendMove { ValueId out; ValueId text; std::vector<ValueId> suffixes; };
struct StringRepeat { ValueId out; ValueId count; ValueId fill; };

template <> struct InstructionTraits<StringIndex> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringIndexAsciiCompare> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringAsciiCountPrefix> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringLength> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringEmpty> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringContains> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringStartsWith> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringEndsWith> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringFind> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringSlice> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringTrim> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringSplit> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringSplitIterBegin> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringSplitIterNext> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringSplitIterEnd> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringParseTwoSigned> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringUtf8> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringFromUtf8> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringFromUtf8ArrayDirect> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringCodepoints> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringJoin> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringConcat> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringBuild> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringBuildAppendMove> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringCanAppendMove> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringAppendMove> : InDomain<Domain::text> {};
template <> struct InstructionTraits<StringRepeat> : InDomain<Domain::text> {};

} // namespace quidra::ir
