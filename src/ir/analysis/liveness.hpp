#pragma once

#include "ir/analysis/alias.hpp"

namespace quidra::ir::analysis {

// Sorted, sparse identities. No array is indexed by an SSA ValueId.
struct LiveSet {
    std::vector<ValueId> values;
    std::vector<std::string> locals;
    std::vector<StorageClassId> storage;
    bool storage_available{true};
    bool operator==(const LiveSet&) const = default;
};
struct FailureEdge {
    InstrRef at;
    BlockId target{};
};
struct LivenessOptions {
    // Exceptional continuations, before the failing instruction defines its
    // outputs. Explicit Branch/FailError paths already belong to the CFG.
    std::vector<FailureEdge> failure_edges;
    // The backend has implicit cleanup paths not represented as IR blocks.
    // Keep cleanup roots at each instruction until precise effects are known.
    bool conservative_failure_cleanup{true};
};
struct Liveness {
    struct Block {
        LiveSet in, out;
        std::vector<LiveSet> before, after;
        bool reachable{};
    };
    std::vector<Block> blocks;
    bool roles_complete{true};
    bool storage_complete{true};
    bool implicit_failures_conservative{true};
    const LiveSet* before(InstrRef) const noexcept;
    const LiveSet* after(InstrRef) const noexcept;
};

// Observational may-liveness for the unchanged typed function and its current
// analyses. Includes lifetime uses, local replacement and exit cleanup, and
// conservative retained/unknown storage. Does not prove uniqueness, exclusive
// state ownership, a valid ownership transfer or permission to donate.
Liveness liveness(const Function&, const Cfg&, const DefUse&, const Slots&,
                  const AliasInfo&, const LivenessOptions& = {}, const LocalSsa* = nullptr);

} // namespace quidra::ir::analysis
