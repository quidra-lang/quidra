#pragma once

// Explicit block control flow, independent of block-label spelling and the
// diagnostic renderer. There is no implicit lexical fallthrough: an absent
// terminator is an abnormal exit, matching the backend's unreachable emission.
// Internal cold failure paths emitted inside nonterminal instructions are not
// edges here; this graph alone is not a liveness,
// cleanup, alias or ownership proof.
//
// Owns all names and graph data. Instructions/blocks are not retained by
// pointer, but indices are valid only while the function remains unchanged.
// Unreachable blocks retain their edges/exits and have no RPO index. Output
// traversal order follows authored blocks and successors, never hash order.

#include "quidra/ir/function.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::ir::analysis {

using BlockId = std::uint32_t;
struct InstrRef {
    BlockId block{};
    std::uint32_t index{}; // SourceLocation instructions count too.
    bool operator==(const InstrRef&) const = default;
};
inline constexpr std::uint32_t kNoRpo = std::numeric_limits<std::uint32_t>::max();

struct Cfg {
    std::vector<std::vector<BlockId>> succ, pred;
    std::vector<BlockId> rpo;
    std::vector<std::uint32_t> rpo_index;
    std::vector<bool> reachable;
    std::vector<BlockId> normal_exits, abnormal_exits;
    std::unordered_map<std::string, BlockId> label;
};

// Rejects duplicate/missing labels and instructions following a terminator
// with invalid_argument. Empty/declaration functions have an empty graph.
Cfg build_cfg(const Function& function);

} // namespace quidra::ir::analysis
