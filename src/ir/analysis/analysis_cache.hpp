#pragma once

#include "ir/analysis/liveness.hpp"
#include "ir/analysis/tensor_facts.hpp"
#include "ir/analysis/loop_invariants.hpp"
#include "ir/analysis/binding_proof.hpp"
#include "quidra/ir/module.hpp"

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>

namespace quidra::ir::analysis {

// One compilation owns a cache. No process-global state or eager analysis.
// Returned references survive other function queries, but not invalidation of
// their function. Every rewrite must invalidate affected functions; changing
// function order, class layouts or call contracts invalidates the whole cache.
class AnalysisCache {
public:
    enum class Kind : std::uint8_t { Cfg, Dominators, PostDominators, DefUse, Slots,
                                     LocalSsa, Aliases, Liveness, TensorFacts, Loops,
                                     LoopInvariants, ObjectProvenance, BindingSources,
                                     BindingProof, Count };
    explicit AnalysisCache(const Module& module) : module_(module) {}
    AnalysisCache(const AnalysisCache&) = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;

    const Cfg& cfg(std::size_t function);
    const DomTree& dominators(std::size_t function);
    const DomTree& post_dominators(std::size_t function, bool include_abnormal = true);
    const DefUse& def_use(std::size_t function);
    const Slots& slots(std::size_t function);
    const LocalSsa& local_ssa(std::size_t function);
    const AliasInfo& aliases(std::size_t function);
    // Default conservative failure cleanup. Custom exceptional continuations
    // use analysis::liveness directly rather than sharing a different cache key.
    const Liveness& liveness(std::size_t function);
    // Only owned, validated formal entry contracts bypass runtime checks.
    // Publishing or replacing a contract invalidates this function's entry.
    const TensorFactAnalysis& tensor_facts(std::size_t function);
    const LoopNest& loops(std::size_t function);
    // Region-specific queries use loop_invariants directly. This cached query
    // has no proposed region membership or physical ownership assumption.
    const LoopInvariants& loop_invariants(std::size_t function);
    // Depends on module-wide source/callee metadata; any function mutation
    // invalidates this observation even when its own body is unchanged.
    const ObjectProvenance& object_provenance(std::size_t function);
    // Owned metadata adaptation is independent of body analysis. Missing
    // concrete lowering origins are explicit unavailable components. Body
    // changes invalidate this owned view before its staleness guards rerun.
    const BindingSources& binding_sources();
    // A module proof reuses each cached per-function observation. References
    // to this proof survive other queries, but no function invalidation.
    const BindingProof& binding_proof();

    void invalidate(std::size_t function);
    void invalidate_all() noexcept;
    std::uint64_t evaluations(Kind kind) const noexcept;

private:
    struct Entry {
        std::optional<Cfg> cfg;
        std::optional<DomTree> dom;
        std::array<std::optional<DomTree>, 2> post;
        std::optional<DefUse> def_use;
        std::optional<Slots> slots;
        std::optional<LocalSsa> local_ssa;
        std::optional<AliasInfo> aliases;
        std::optional<Liveness> liveness;
        std::optional<TensorFactAnalysis> tensor_facts;
        std::optional<LoopNest> loops;
        std::optional<LoopInvariants> loop_invariants;
        std::optional<ObjectProvenance> object_provenance;
    };
    Entry& entry(std::size_t function);
    void evaluated(Kind kind) noexcept;
    const Module& module_;
    std::unordered_map<std::size_t, std::unique_ptr<Entry>> entries_;
    std::optional<BindingSources> binding_sources_;
    std::optional<BindingProof> binding_proof_;
    std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> evaluations_{};
};

} // namespace quidra::ir::analysis
