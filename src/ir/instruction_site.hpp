#pragma once

// Common IR instruction-site model. Sites index one unchanged typed
// function; they carry no CFG, reachability or analysis result.
#include <cstdint>

// Retain the established namespace for source and ABI compatibility.
namespace quidra::ir::analysis {
using BlockId = std::uint32_t;
struct InstrRef {
    BlockId block{};
    std::uint32_t index{}; // SourceLocation instructions count too.
    bool operator==(const InstrRef&) const = default;
};
} // namespace quidra::ir::analysis
