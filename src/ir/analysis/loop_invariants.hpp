#pragma once

#include "ir/analysis/loops.hpp"
#include "ir/analysis/liveness.hpp"
#include "ir/analysis/tensor_facts.hpp"

namespace quidra::ir::analysis {
enum class InvariantReason : std::uint8_t {
    UnknownCall, UnknownMutation, ReferenceUnavailable, AliasUnavailable,
    StateIdentityUnavailable, WrittenInLoop, DescriptorUnavailable,
    EntryUnavailable, LoopUnavailable, OutsideRegionWrite, BackedgeLive,
    ExposedOrEscaped, PolicyUnavailable
};
struct MetadataInvariant {
    bool shape{}, dtype{}, device{}, contiguous{}, tracked{}, fully_initialized{};
};
struct LocalInvariant {
    bool entry_available{}, value_unchanged{};
    MetadataInvariant metadata;
    std::optional<std::uint32_t> region_only_writes;
    std::vector<InvariantReason> unavailable;
};
struct StorageInvariant {
    StorageClassId storage{};
    bool payload_unchanged{}, descriptor_unchanged{}, state_identity_available{};
    std::optional<std::uint32_t> region_only_writes;
    bool backedge_live_available{}, outside_region_live{};
    // Membership/owned bits are not the later physical ownership verifier.
    bool exclusive_ownership_available{};
    std::vector<InvariantReason> unavailable;
};
struct CandidateRegion {
    std::uint32_t id{};
    InstrRef entry;
    std::vector<InstrRef> instructions; // Concrete current-function membership only.
};
struct LoopInvariantInputs {
    const DefUse& uses;
    const LocalSsa& ssa;
    const Slots& slots;
    const AliasInfo& aliases;
    const Liveness& live;
    const ModuleSummaries& summaries;
    const TensorFactAnalysis* tensors{};
    const facts::FactSources* sources{};
    std::vector<CandidateRegion> regions{};
};
struct InvariantLoop {
    std::uint32_t loop{};
    std::vector<ValueId> values; // Logical contents, not allocation/descriptor identity or hoisting permission.
    std::map<std::string,LocalInvariant,std::less<>> locals;
    std::vector<StorageInvariant> storage;
    facts::PolicyFacts policies;
    bool policy_available{}, policy_unchanged{}, state_complete{};
    std::vector<InvariantReason> unavailable;
};
struct LoopInvariants {std::vector<InvariantLoop> loops;};
// Observational facts for the unchanged verified function. Unknown alias,
// exposed reference, opaque call or persistent identity is not a no-write
// proof. No instruction motion, error-order, termination or donation proof.
LoopInvariants loop_invariants(const Function&,const Cfg&,const DomTree&,const LoopNest&,
                              const LoopInvariantInputs&);
} // namespace quidra::ir::analysis
