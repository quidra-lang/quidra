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

// Fuse operation chains until none is left, then apply one conditional
// rule; after a rule, fuse again.
inline constexpr std::array<Pass, 2> rewrite_passes{{
    {"chain-fusion", always, ChainFusionPass::run},
    {"conditional-rules", always, ConditionalRulePass::run},
}};

// The regions are formed and annotated after the rewrites, so that they
// describe the rewritten module; no rewrite reads them.
inline constexpr std::array<PipelineEntry, 3> pipeline{{
    FixpointGroup{"rewrites", rewrite_passes},
    Pass{"region-formation", always, DataflowComponentRegions::run},
    Pass{"region-candidates", always, RegionCandidateAnnotation::run},
}};

} // namespace quidra::optimizer
