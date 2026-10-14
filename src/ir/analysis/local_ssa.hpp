#pragma once

#include "ir/analysis/dominators.hpp"

#include <optional>
#include <string_view>
#include <unordered_map>

namespace quidra::ir::analysis {

enum class SlotClass : std::uint8_t {
    Promotable, AddressTaken, ReferenceBound, WritableParam, BorrowedParam, BorrowedLocal, OpaqueAccess
};
// Independent facts retain overlapping exposure and ownership restrictions.
// Promotion is not proof of storage ownership or permission to donate.
struct SlotFacts {
    bool writable_parameter{}, borrowed_parameter{}, borrowed_local{};
    bool address_taken{}, reference_bound{}, reference_binding{}, move_out{};
    bool opaque_access{}; // Named intrinsic access not modeled by this overlay.
    bool promotable() const noexcept;
};
struct Slots {
    std::unordered_map<std::string, SlotClass> cls;
    std::unordered_map<std::string, Type> type;
    std::unordered_map<std::string, SlotFacts> facts;
    std::vector<std::string> ordered; // Parameters, then authored storage declarations/stores.
    bool promotable(std::string_view local) const;
};
Slots classify_slots(const Function& function);

// Analysis identity only; never an IR ValueId or Function::next_value.
struct PhiId {
    std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};
    bool operator==(const PhiId&) const = default;
};
struct ReachingDef {
    enum Kind : std::uint8_t { Undefined, Param, Store, Phi, Empty } kind{Undefined};
    ValueId value{}; // Store payload only.
    InstrRef store{}; // Store or move-out Empty origin.
    PhiId phi{};
    std::uint32_t parameter{std::numeric_limits<std::uint32_t>::max()};
    bool available{}; // Undefined on reachable paths is distinct from no proof.
    std::optional<Type> empty_type; // Actual typed empty value after move-out.
    bool operator==(const ReachingDef&) const = default;
};
struct LocalSsa {
    struct Phi {
        BlockId block{};
        std::string local;
        PhiId id;
        std::vector<std::pair<BlockId, ReachingDef>> incoming; // Exact CFG pred order.
        std::optional<ReachingDef> entry; // External entry, separate from a root backedge.
        bool operator==(const Phi&) const = default;
    };
    std::vector<Phi> phis;
    // Every promotable LoadLocal, including unavailable unreachable loads.
    std::unordered_map<ValueId, ReachingDef> load_source;
    // Before a valid instruction, or at index==instruction_count for its
    // block's end boundary, of the unchanged input function. Unknown,
    // nonpromotable, unreachable and out-of-range sites are unavailable.
    // A dead ambiguous state whose phi was pruned is unavailable, too.
    ReachingDef reaching(InstrRef at, std::string_view local) const;

private:
    struct BlockSources {
        std::optional<ReachingDef> phi;
        std::vector<std::pair<std::uint32_t, ReachingDef>> definitions;
        std::vector<std::pair<std::uint32_t, ReachingDef>> loads;
    };
    std::unordered_map<std::string, ReachingDef> initial_;
    std::unordered_map<std::string, std::unordered_map<BlockId, BlockSources>> sources_;
    std::vector<std::vector<BlockId>> predecessors_;
    std::vector<bool> reachable_;
    std::vector<std::uint32_t> instruction_count_;
    friend LocalSsa build_local_ssa(const Function&, const Cfg&, const DomTree&, const Slots&);
};

// Inputs are current build_cfg/dominators/classify_slots results for the same
// verified typed function. Entry-path scope only; no unreachable, lifetime,
// initialization-flag, cleanup or donation proof. Owns all query metadata.
// Allocation/validation exceptions propagate without mutating inputs.
LocalSsa build_local_ssa(const Function&, const Cfg&, const DomTree&, const Slots&);

} // namespace quidra::ir::analysis
