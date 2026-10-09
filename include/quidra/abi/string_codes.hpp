#pragma once

// The part kinds of the runtime's string builder (quidra_string_build and
// quidra_string_build_append_move_unique_direct), which generated code writes
// as one byte per part and the runtime dispatches on.

namespace quidra::abi {

namespace string_build_part_kind {
// A string (or an error's message).
inline constexpr int text = 0;
// A signed integer, formatted in decimal.
inline constexpr int signed_integer = 1;
// An unsigned integer, formatted in decimal.
inline constexpr int unsigned_integer = 2;
// A bool, formatted as true or false.
inline constexpr int boolean = 3;
// A string the compiler proved to be one ASCII character.
inline constexpr int ascii_character = 4;
} // namespace string_build_part_kind

} // namespace quidra::abi
