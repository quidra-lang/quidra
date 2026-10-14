#pragma once

// Typed-IR instructions of the text domain (strings).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/slice.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <vector>

namespace quidra::ir {

struct StringIndex { ValueId out; ValueId text; ValueId index; SourcePosition position{}; };
struct StringIndexAsciiCompare { ValueId out; ValueId text; ValueId index; unsigned char byte{}; bool negate{}; SourcePosition position{}; };
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
struct StringSlice { ValueId out; ValueId text; ValueId start; ValueId end; SourcePosition position{}; };
// A slice of a string in any other form than start:<end: the code
// points it selects, in its order.
struct StringSliceStepped { ValueId out; ValueId text; SliceBounds bounds; SourcePosition position{}; };
struct StringTrim { ValueId out; ValueId text; };
struct StringSplit { ValueId out; ValueId text; ValueId separator; };
struct StringSplitIterBegin { ValueId out; ValueId text; ValueId separator; bool move_source{}; };
struct StringSplitIterNext { ValueId text; ValueId has_value; ValueId cursor; };
struct StringSplitIterEnd { ValueId cursor; };
struct StringParseTwoSigned { ValueId left; ValueId right; ValueId ok; ValueId text; unsigned char separator{}; };
struct StringUtf8 { ValueId out; ValueId text; };
struct StringFromUtf8 { ValueId out; ValueId bin; Type result_type; SourcePosition position{}; };
struct StringFromUtf8ArrayDirect { ValueId text; ValueId ok; ValueId error; ValueId array; SourcePosition position{}; };
struct StringCodepoints { ValueId out; ValueId text; };
struct StringJoin { ValueId out; ValueId values; ValueId separator; SourcePosition position{}; };
struct StringConcat { ValueId out; std::vector<ValueId> values; };
enum class StringBuildPartKind { Plain, QuotedString };
struct StringBuildPart { ValueId value; Type type; bool single_byte_ascii{}; StringBuildPartKind kind{StringBuildPartKind::Plain}; };
struct StringQuote {ValueId out;ValueId text;};
struct StringBuild { ValueId out; std::vector<StringBuildPart> parts; ValueId separator; };
struct StringBuildAppendMove { ValueId out; ValueId added_length; ValueId text; std::vector<StringBuildPart> parts; ValueId separator; };
struct StringCanAppendMove { ValueId out; ValueId text; };
struct StringAppendMove { ValueId out; ValueId text; std::vector<ValueId> suffixes; };
struct StringRepeat { ValueId out; ValueId count; ValueId fill; };

template <> struct InstructionTraits<StringIndex> : WithOperands<Domain::text, StringIndex> {};
template <> struct InstructionTraits<StringIndexAsciiCompare> : WithOperands<Domain::text, StringIndexAsciiCompare> {};
template <> struct InstructionTraits<StringAsciiCountPrefix> : WithOperands<Domain::text, StringAsciiCountPrefix> {};
template <> struct InstructionTraits<StringLength> : WithOperands<Domain::text, StringLength> {};
template <> struct InstructionTraits<StringEmpty> : WithOperands<Domain::text, StringEmpty> {};
template <> struct InstructionTraits<StringContains> : WithOperands<Domain::text, StringContains> {};
template <> struct InstructionTraits<StringStartsWith> : WithOperands<Domain::text, StringStartsWith> {};
template <> struct InstructionTraits<StringEndsWith> : WithOperands<Domain::text, StringEndsWith> {};
template <> struct InstructionTraits<StringFind> : WithOperands<Domain::text, StringFind> {};
template <> struct InstructionTraits<StringSlice> : WithOperands<Domain::text, StringSlice> {};
template <> struct InstructionTraits<StringSliceStepped> : WithOperands<Domain::text, StringSliceStepped> {};
template <> struct InstructionTraits<StringTrim> : WithOperands<Domain::text, StringTrim> {};
template <> struct InstructionTraits<StringSplit> : WithOperands<Domain::text, StringSplit> {};
template <> struct InstructionTraits<StringSplitIterBegin> : WithOperands<Domain::text, StringSplitIterBegin> {};
template <> struct InstructionTraits<StringSplitIterNext> : WithOperands<Domain::text, StringSplitIterNext> {};
template <> struct InstructionTraits<StringSplitIterEnd> : WithOperands<Domain::text, StringSplitIterEnd> {};
template <> struct InstructionTraits<StringParseTwoSigned> : WithOperands<Domain::text, StringParseTwoSigned> {};
template <> struct InstructionTraits<StringUtf8> : WithOperands<Domain::text, StringUtf8> {};
template <> struct InstructionTraits<StringFromUtf8> : WithOperands<Domain::text, StringFromUtf8> {};
template <> struct InstructionTraits<StringFromUtf8ArrayDirect> : WithOperands<Domain::text, StringFromUtf8ArrayDirect> {};
template <> struct InstructionTraits<StringCodepoints> : WithOperands<Domain::text, StringCodepoints> {};
template <> struct InstructionTraits<StringJoin> : WithOperands<Domain::text, StringJoin> {};
template <> struct InstructionTraits<StringConcat> : WithOperands<Domain::text, StringConcat> {};
template <> struct InstructionTraits<StringQuote> : WithOperands<Domain::text, StringQuote> {};
template <> struct InstructionTraits<StringBuild> : WithOperands<Domain::text, StringBuild> {};
template <> struct InstructionTraits<StringBuildAppendMove> : WithOperands<Domain::text, StringBuildAppendMove> {};
template <> struct InstructionTraits<StringCanAppendMove> : WithOperands<Domain::text, StringCanAppendMove> {};
template <> struct InstructionTraits<StringAppendMove> : WithOperands<Domain::text, StringAppendMove> {};
template <> struct InstructionTraits<StringRepeat> : WithOperands<Domain::text, StringRepeat> {};

} // namespace quidra::ir
