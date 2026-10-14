#include "ir/analysis/analysis_cache.hpp"
#include "ir/facts/entry.hpp"

#include <stdexcept>

namespace quidra::ir::analysis {
AnalysisCache::Entry& AnalysisCache::entry(std::size_t function) {
    if (function >= module_.functions.size()) throw std::out_of_range("IR analysis function index is out of range");
    const auto found = entries_.find(function);
    if (found != entries_.end()) return *found->second;
    auto value = std::make_unique<Entry>();
    return *entries_.emplace(function, std::move(value)).first->second;
}
void AnalysisCache::evaluated(Kind kind) noexcept {
    auto& counter = evaluations_[static_cast<std::size_t>(kind)];
    if (counter != std::numeric_limits<std::uint64_t>::max()) ++counter;
}
std::uint64_t AnalysisCache::evaluations(Kind kind) const noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < evaluations_.size() ? evaluations_[index] : 0;
}
const Cfg& AnalysisCache::cfg(std::size_t function) {
    auto& value = entry(function).cfg;
    if (!value) { value = build_cfg(module_.functions[function]); evaluated(Kind::Cfg); }
    return *value;
}
const DomTree& AnalysisCache::dominators(std::size_t function) {
    auto& value = entry(function).dom;
    if (!value) { value = analysis::dominators(cfg(function)); evaluated(Kind::Dominators); }
    return *value;
}
const DomTree& AnalysisCache::post_dominators(std::size_t function, bool abnormal) {
    auto& value = entry(function).post[abnormal ? 1 : 0];
    if (!value) { value = analysis::post_dominators(cfg(function), abnormal); evaluated(Kind::PostDominators); }
    return *value;
}
const DefUse& AnalysisCache::def_use(std::size_t function) {
    auto& value = entry(function).def_use;
    if (!value) { value = analysis::def_use(module_.functions[function], &module_); evaluated(Kind::DefUse); }
    return *value;
}
const Slots& AnalysisCache::slots(std::size_t function) {
    auto& value = entry(function).slots;
    if (!value) { value = classify_slots(module_.functions[function]); evaluated(Kind::Slots); }
    return *value;
}
const LocalSsa& AnalysisCache::local_ssa(std::size_t function) {
    auto& value = entry(function).local_ssa;
    if (!value) {
        value = build_local_ssa(module_.functions[function], cfg(function), dominators(function), slots(function));
        evaluated(Kind::LocalSsa);
    }
    return *value;
}
const AliasInfo& AnalysisCache::aliases(std::size_t function) {
    auto& value = entry(function).aliases;
    if (!value) {
        value = alias_analysis(module_.functions[function], def_use(function), local_ssa(function), slots(function), &module_);
        evaluated(Kind::Aliases);
    }
    return *value;
}
const Liveness& AnalysisCache::liveness(std::size_t function) {
    auto& value = entry(function).liveness;
    if (!value) {
        value = analysis::liveness(module_.functions[function], cfg(function), def_use(function), slots(function),
                                   aliases(function), {}, &local_ssa(function));
        evaluated(Kind::Liveness);
    }
    return *value;
}
const TensorFactAnalysis& AnalysisCache::tensor_facts(std::size_t function) {
    auto& value = entry(function).tensor_facts;
    if (!value) {
        const ModuleSummaries empty_summaries;
        const facts::FactSources empty_sources;
        ParamFacts parameters;
        const auto& fn = module_.functions[function];
        if (fn.validated_entry && fn.declaration &&
            same_declaration(*fn.declaration, fn.validated_entry->declaration) &&
            fn.declaration->generic_args == fn.validated_entry->declaration.generic_args) {
            const auto& contract = *fn.validated_entry;
            for (const auto& [index, fact] : contract.tensors) {
                if (index >= fn.parameters.size() || fn.parameters[index].writable || fn.parameters[index].type.kind != TypeKind::Tensor ||
                    !fact.has_type || !fact.type.first || !fn.parameters[index].type.first ||
                    *fact.type.first != *fn.parameters[index].type.first) continue;
                auto formal = fact;
                // A caller contract never certifies a callee's storage owner,
                // refcount or initialization. Those need local proofs.
                formal.unique = facts::Tri::Unknown;
                formal.owns_storage = false;
                formal.fully_initialized = facts::Tri::Unknown;
                parameters.tensors.emplace(index, std::move(formal));
            }
            for (const auto& [index, extent] : contract.scalars)
                if (index < fn.parameters.size() && !fn.parameters[index].writable &&
                    is_integer_family_type(fn.parameters[index].type) && extent.known())
                    parameters.scalars.emplace(index, extent);
            parameters.policies = contract.policies;
        }
        value = compute_tensor_facts(module_.functions[function], cfg(function), dominators(function),
            local_ssa(function), slots(function),
            module_.summaries ? *module_.summaries : empty_summaries,
            module_.fact_sources ? *module_.fact_sources : empty_sources, parameters, &aliases(function));
        evaluated(Kind::TensorFacts);
    }
    return *value;
}
const LoopNest& AnalysisCache::loops(std::size_t function) {
    auto& value = entry(function).loops;
    if (!value) {
        value = build_loops(cfg(function), dominators(function));
        evaluated(Kind::Loops);
    }
    return *value;
}
const LoopInvariants& AnalysisCache::loop_invariants(std::size_t function) {
    auto& value = entry(function).loop_invariants;
    if (!value) {
        const ModuleSummaries empty_summaries;
        const auto& summary = module_.summaries ? *module_.summaries : empty_summaries;
        const LoopInvariantInputs inputs{def_use(function), local_ssa(function), slots(function),
            aliases(function), liveness(function), summary, &tensor_facts(function), module_.fact_sources.get(), {}};
        value = analysis::loop_invariants(module_.functions[function], cfg(function), dominators(function),
            loops(function), inputs);
        evaluated(Kind::LoopInvariants);
    }
    return *value;
}
void AnalysisCache::invalidate(std::size_t function) { entries_.erase(function); }
void AnalysisCache::invalidate_all() noexcept { entries_.clear(); }
} // namespace quidra::ir::analysis
