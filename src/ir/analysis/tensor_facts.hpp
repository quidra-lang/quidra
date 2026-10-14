#pragma once

#include "ir/analysis/local_ssa.hpp"
#include "ir/fact_sources.hpp"

namespace quidra::ir::analysis {
struct AliasInfo;
using ir::facts::TensorFacts;
using ir::facts::PolicyFacts;
struct ParamFacts {
    // Explicit validated entry facts; declared signature constraints alone
    // cannot prove the entry checks that establish them.
    std::map<std::size_t, TensorFacts> tensors;
    std::map<std::size_t, ShapeExtent> scalars;
    PolicyFacts policies;
};
struct TensorFactKey {
    enum Kind : std::uint8_t { Value, Parameter, Phi } kind{Value};
    std::uint32_t id{};
    auto operator<=>(const TensorFactKey&) const = default;
};
struct TensorFactScope {
    std::map<TensorFactKey, TensorFacts> incoming;
    // A check's facts start at index+1, so its own before-site has no proof.
    std::map<TensorFactKey, std::vector<std::pair<std::uint32_t, TensorFacts>>> changes;
    std::map<TensorFactKey, ShapeExtent> scalar_incoming;
    std::map<TensorFactKey, std::vector<std::pair<std::uint32_t, ShapeExtent>>> scalar_changes;
    std::vector<PolicyFacts> policies_before;
};
struct TensorFactAnalysis {
    std::map<ir::ValueId, TensorFacts> values; // Intrinsic definition facts.
    std::map<ir::ValueId, ShapeExtent> scalars;
    std::map<std::uint32_t, TensorFacts> phis;
    std::vector<TensorFactScope> scopes;
    std::map<ir::ValueId, TensorFactKey> lineage;
    std::map<ir::ValueId, ir::analysis::InstrRef> definitions;
    ir::analysis::DomTree dominance;
    std::vector<bool> reachable;
    TensorFacts at(ir::analysis::InstrRef, ir::ValueId) const;
    ShapeExtent scalar_at(ir::analysis::InstrRef, ir::ValueId) const;
    const PolicyFacts* policy_at(ir::analysis::InstrRef) const noexcept;
};
TensorFactAnalysis compute_tensor_facts(const ir::Function&, const ir::analysis::Cfg&,
    const ir::analysis::DomTree&, const ir::analysis::LocalSsa&, const ir::analysis::Slots&,
    const ir::analysis::ModuleSummaries&, const ir::facts::FactSources&, const ParamFacts& = {},
    const AliasInfo* = nullptr);
} // namespace quidra::ir::analysis
