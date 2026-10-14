#pragma once

// The optimizer, top-down: ir::optimize() runs these entries in order on its
// module (PassManager). A Pass runs once. A FixpointGroup runs its passes in
// order and starts again from its first pass whenever one reports a rewrite,
// until a round rewrites nothing. A pass has a name, a predicate that
// enables it, and the function that runs it, which returns whether it
// changed instructions.
//
// Every pass keeps the observable trace (docs/spec/architecture.md, the
// optimizer contract): the outputs and their order, the identity and
// location of the first error, and the final state. A pass may reorder,
// fuse, overlap or defer operations only within it; two operations that may
// fail are reordered only when at most one can fail or the first error in
// source order is still the one reported.

#include "optimizer/chain_fusion.hpp"
#include "optimizer/conditional_rules.hpp"
#include "optimizer/finite_check_merging.hpp"
#include "optimizer/pass_context.hpp"
#include "optimizer/region_candidates.hpp"
#include "optimizer/region_formation.hpp"

#include "quidra/ir/module.hpp"

#include <array>
#include <span>
#include <string_view>
#include <variant>

namespace quidra::optimizer {

struct Pass {
    std::string_view name;
    bool (*enabled)(const PassContext&);
    bool (*run)(ir::Module&, const PassContext&);
};

struct FixpointGroup {
    std::string_view name;
    std::span<const Pass> passes;
};

using PipelineEntry = std::variant<Pass, FixpointGroup>;

constexpr bool always(const PassContext&) { return true; }

bool run_rule_dispatch(ir::Module&, const PassContext&);
bool run_helper_specialization(ir::Module&, const PassContext&);
bool run_budgeted_inlining(ir::Module&, const PassContext&);
constexpr bool interproc_on(const PassContext& context) {
    return context.options.exact_binding == DeclarationBindingMode::On;
}

// Fuse operation chains until none is left, then apply one conditional
// rule; after a rule, fuse again.
inline constexpr std::array<Pass, 2> rewrite_passes{{
    {"chain-fusion", always, ChainFusionPass::run},
    {"conditional-rules", always, ConditionalRulePass::run},
}};

// The finite checks of real arithmetic are merged after the rewrites,
// which do not move scalar operations. The regions are formed and annotated
// last, so that they describe the rewritten module; no rewrite reads them.
inline constexpr std::array<PipelineEntry, 7> pipeline{{
    Pass{"helper-specialization", interproc_on, run_helper_specialization},
    Pass{"budgeted-inlining", interproc_on, run_budgeted_inlining},
    FixpointGroup{"rewrites", rewrite_passes},
    Pass{"rule-dispatch", always, run_rule_dispatch},
    Pass{"finite-check-merging", FiniteCheckMerging::enabled, FiniteCheckMerging::run},
    Pass{"region-formation", always, DataflowComponentRegions::run},
    Pass{"region-candidates", always, RegionCandidateAnnotation::run},
}};

} // namespace quidra::optimizer
