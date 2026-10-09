#pragma once

// The memory layouts of values that generated code and the runtime both read
// and write.
//
// Owns: the header of a dynamic array and of a bin, the union box, the
// exact real value, the word of a bare integer, the source provenance
// record of a statement that can fail at run time, and the source table of a
// program's user files. The offsets are ints, like the literals they name, so
// expressions that use them keep their types.

#include <cstddef>

namespace quidra::abi {

// A dynamic array: its element count as an i64 at offset 0, then the elements
// from payload_offset. A fixed-size array has no header.
namespace array_layout {
inline constexpr int payload_offset = 8;
} // namespace array_layout

// A bin: its length in bits as an i64 at offset 0, then the bytes from
// payload_offset, the first bit in the most significant bit of the first
// byte.
namespace bin_layout {
inline constexpr int payload_offset = 8;
} // namespace bin_layout

// A union box, the boxed value of a union type (a recoverable result
// T | error, ...): its case tag as an i64 at offset 0, then the case's value
// from payload_offset. A box takes `bytes`, or payload_offset plus the size
// of its largest case when that is larger (a case of type `real`).
namespace union_box_layout {
inline constexpr int payload_offset = 8;
inline constexpr int bytes = 16;
} // namespace union_box_layout

// A value of the exact type `real`: two i64 words, the numerator at offset 0
// and the denominator at offset 8. With a denominator of at least 1 the
// value is the small rational numerator/denominator, always in lowest terms,
// with 0 as 0/1 and a numerator other than INT64_MIN. With a denominator of
// 0 it is the general form: the numerator holds the address of the
// runtime's managed node of the value (0 for no value). A value that has a
// small form is always stored in it.
struct ExactRealValue {
    long long numerator;
    long long denominator;
};

namespace exact_real_layout {
inline constexpr int numerator_offset = 0;
inline constexpr int denominator_offset = 8;
inline constexpr int bytes = 16;
} // namespace exact_real_layout

static_assert(sizeof(ExactRealValue) == exact_real_layout::bytes &&
                  sizeof(long long) == exact_real_layout::denominator_offset,
              "an exact real is two adjacent i64 words without padding");

// A value of an arbitrary-precision integer type: one i64 word. With the low
// bit clear it is inline, and the value is the word shifted right by one
// (arithmetically), within [inline_min, inline_max]. With the low bit set it
// is boxed: the word without that bit is the address of the runtime's
// managed big integer. Every value within the inline range is inline, so two
// words of which one is inline are equal exactly when their values are, and
// inline words order like their values.
namespace bare_integer_layout {
inline constexpr long long boxed_bit = 1;
inline constexpr long long inline_min = -(1LL << 62);
inline constexpr long long inline_max = (1LL << 62) - 1;
inline constexpr int bytes = 8;
} // namespace bare_integer_layout

// The record (@.quidra.source.N) that generated code passes to
// quidra_runtime_set_source_provenance for the statement that runs next: the
// source file, its revision, the statement's node id and kind, and its line
// and column. The backend writes it as { ptr, ptr, ptr, ptr, i64, i64 }.
struct SourceProvenanceRecord {
    const char* file{};
    const char* revision{};
    const char* node_id{};
    const char* node_kind{};
    unsigned long long line{};
    unsigned long long column{};
};

static_assert(sizeof(SourceProvenanceRecord) ==
                  4 * sizeof(const char*) + 2 * sizeof(unsigned long long),
              "the record has no padding: four pointers, then two i64 fields");

// One user file of the program, in its source table: the path to show (as
// the user spelled the root, imports relative to it), the absolute path (the
// same constant as the `file` of the file's provenance records), the
// SHA-256 of its bytes as 64 lowercase hex digits (empty: no file, as in the
// REPL), its line count (line feeds + 1) and its size in bytes. The backend
// writes it as { ptr, ptr, ptr, i64, i64 }.
struct SourceTableEntry {
    const char* display{};
    const char* absolute{};
    const char* revision{};
    unsigned long long line_count{};
    unsigned long long byte_size{};
};

static_assert(offsetof(SourceTableEntry, line_count) ==
                  ((3 * sizeof(const char*) + alignof(unsigned long long) - 1) /
                   alignof(unsigned long long)) * alignof(unsigned long long) &&
                  offsetof(SourceTableEntry, byte_size) ==
                      offsetof(SourceTableEntry, line_count) + sizeof(unsigned long long) &&
                  sizeof(SourceTableEntry) ==
                      offsetof(SourceTableEntry, byte_size) + sizeof(unsigned long long),
              "source-table fields follow target pointer and i64 alignment");

// The source table (@.quidra.sources) that generated code registers before
// its first statement (quidra_runtime_register_sources): the number of
// entries as an i64, then the entries, the root file first. A site's line
// immediate names an entry by its index in the upper 32 bits. The backend
// writes it as { i64, [count x SourceTableEntry] }.
struct SourceTable {
    unsigned long long count{};
};

static_assert(sizeof(SourceTable) == 8 && alignof(SourceTableEntry) <= 8,
              "the entries follow the count without padding");

// The entries that follow the table's count.
inline const SourceTableEntry* source_table_entries(const SourceTable* table) {
    return reinterpret_cast<const SourceTableEntry*>(table + 1);
}

} // namespace quidra::abi
