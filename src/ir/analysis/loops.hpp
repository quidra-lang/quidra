#pragma once

#include "ir/analysis/dominators.hpp"

namespace quidra::ir::analysis {
struct LoopExit {
    BlockId from{}, to{};
    bool operator==(const LoopExit&) const = default;
};
struct Loop {
    BlockId header{};
    std::optional<BlockId> preheader;
    std::vector<BlockId> latches, blocks;
    std::vector<LoopExit> exits;
    std::optional<std::uint32_t> parent;
    std::uint32_t depth{};
    bool available{true}; // False for overlapping, non-laminar natural sets.
};
struct IrreducibleComponent {
    std::vector<BlockId> blocks, entries;
};
struct LoopNest {
    std::vector<Loop> loops; // Authored header order; children refer by index.
    std::vector<std::int32_t> innermost_of_block; // -1 outside a proven loop.
    std::vector<IrreducibleComponent> irreducible;
    std::vector<bool> reachable, loop_available;
};
// Reachable entry paths only. Abnormal/cold failure paths are not invented;
// no termination, hoisting, ownership or error-order proof. Iterative graph
// walks also expose nested irreducible components inside reducible SCCs.
LoopNest build_loops(const Cfg&, const DomTree&);
} // namespace quidra::ir::analysis
