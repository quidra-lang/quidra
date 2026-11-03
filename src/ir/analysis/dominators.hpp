#pragma once

#include "ir/analysis/cfg.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace quidra::ir::analysis {

enum class DomScope : std::uint8_t {
    EntryPaths,
    // Only finite paths to the selected exits participate. This relation
    // does not prove termination, absence of failure, cleanup or ownership.
    FiniteSelectedExitPaths
};

struct DomTree {
    // Root dominates itself. Unavailable nodes have idom=-1 and pre/post
    // kNoRpo, with no children/frontier. Traversals never use label/hash order.
    std::vector<std::int32_t> idom;
    std::vector<std::vector<BlockId>> children, frontier;
    std::vector<std::uint32_t> pre, post;
    std::optional<BlockId> root;
    BlockId block_count{}; // Original blocks; excludes the virtual exit.
    DomScope scope{DomScope::EntryPaths};
    bool includes_abnormal_exits{};

    bool dominates(BlockId ancestor, BlockId descendant) const noexcept;
    // References must be valid for the unchanged function that produced Cfg.
    // Same-block order reverses for postdominance. Virtual exit has no InstrRef.
    bool dominates(InstrRef ancestor, InstrRef descendant) const noexcept;
};

// Consumes a Cfg from build_cfg for an unchanged function. Forward dominance
// excludes entry-unreachable blocks. CHK uses deterministic reverse postorder.
DomTree dominators(const Cfg& cfg);

// Nonempty graphs expose virtual exit block_count in the result arrays. It
// connects entry-reachable normal exits, and optionally abnormal exits.
// Blocks with no finite path to such an exit (including no-exit infinite SCCs)
// are unavailable. Infinite paths and excluded abnormal exits are outside this
// relation: a finite-exit postdominator is not an all-maximal-path proof.
DomTree post_dominators(const Cfg& cfg, bool abnormal_exits_reach_virtual_exit = true);

} // namespace quidra::ir::analysis
