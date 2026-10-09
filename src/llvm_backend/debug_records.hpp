#pragma once

// Debug information records and the text passes that attach !dbg locations.
//
// Owns: what one function's emission records for the module's debug metadata
// (DebugLocationRecord for a statement location, DebugVariableRecord and its
// DebugBasicType for a local or parameter), the DWARF basic type of a scalar
// Quidra type, and the two passes over finished function text that append
// ", !dbg !N": to the first instruction of the function, and to the first
// instruction line of a byte range (a statement's segment). The passes edit
// text by byte offset, so they run after the function text is complete.

#include "quidra/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace quidra::llvm_backend {

// Appends ", !dbg !<location>" to the first instruction after the first label.
std::string attach_debug_location(std::string text,std::size_t location);

struct DebugLocationRecord {
    std::size_t id{};
    std::size_t scope{};
    std::uint32_t line{};
    std::uint32_t column{};
};

struct DebugBasicType {
    std::string name;
    std::size_t bits{};
    const char* encoding{};
};

struct DebugVariableRecord {
    std::size_t id{};
    std::size_t type_id{};
    std::size_t scope{};
    std::size_t file{};
    std::string name;
    DebugBasicType type;
    std::uint32_t line{};
    std::size_t argument{};
};

// The DWARF basic type of a scalar type; none for other types.
std::optional<DebugBasicType> debug_basic_type(const Type& type);

// Appends ", !dbg !<location>" to the first instruction line in
// [begin, end) that is not an operand continuation and has no !dbg yet;
// returns whether it found one.
bool attach_debug_location_range(
    std::string& text,std::size_t begin,std::size_t end,std::size_t location);

} // namespace quidra::llvm_backend
