#include "optimizer/foundation_passes.hpp"
#include "optimizer/check_elimination.hpp"
#include "optimizer/inspection.hpp"
#include "ir/analysis/step_region_annotations.hpp"

#include <algorithm>
#include <utility>

namespace quidra::optimizer {
namespace {
bool run_checks(ir::Module& module, const PassContext& context, std::uint64_t round) {
    auto* inspection = context.options.inspection;
    const bool capture = inspection && inspection->capture_proofs;
    if (capture) inspection->proofs_observed = true;
    std::uint64_t remaining = 2'000'000;
    bool changed = false;
    for (std::size_t function = 0; function < module.functions.size(); ++function) {
        ProofInspection row;
        if (capture) {
            row.function = function;
            row.lookup = module.functions[function].name;
            row.declaration = module.functions[function].declaration;
            row.round = round;
            row.site_context = module.functions[function];
        }
        CheckEliminationOptions options;
        options.maximum_operations = remaining;
        const auto plan = CheckElimination::analyze(module, function, options);
        remaining -= std::min(remaining, plan.report().operations);
        if (capture) {
            row.ready = plan.ready();
            row.report = plan.report();
        }
        const bool publish = context.options.check_elimination == ir::OptimizerMode::On &&
            context.options.cfg_facts.value_or(ir::OptimizerMode::On) == ir::OptimizerMode::On;
        if (publish && plan.ready() && !plan.report().certificates.empty()) {
            const auto applied = CheckElimination::apply(module, plan, remaining);
            remaining -= std::min(remaining, applied.operations);
            changed |= applied.applied && (applied.removed || applied.flagged);
            if (capture) {
                row.application_observed = true;
                row.application = applied;
            }
        }
        if (capture) inspection->proofs.push_back(std::move(row));
        if (!remaining || plan.report().failure == CheckGap::Budget) {
            if (context.options.diagnostics)
                context.options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableAnalysis,
                    module.functions[function].name, "check-elimination operation budget exhausted", {}, {}});
            break;
        }
    }
    return changed;
}
}
bool run_check_elimination_first(ir::Module& module, const PassContext& context) {
    return run_checks(module, context, 1);
}
bool run_check_elimination_second(ir::Module& module, const PassContext& context) {
    return run_checks(module, context, 2);
}
bool run_step_region_annotations(ir::Module& module, const PassContext& context) {
    auto* inspection = context.options.inspection;
    const bool capture = inspection && inspection->capture_step_regions;
    const auto built = ir::analysis::build_step_region_annotations(module);
    const bool publish = context.options.step_regions == ir::OptimizerMode::On &&
        context.options.cfg_facts.value_or(ir::OptimizerMode::On) == ir::OptimizerMode::On;
    if (capture) {
        inspection->step_regions_observed = true;
        inspection->step_regions_ready = static_cast<bool>(built.owner);
        inspection->step_regions_upstream_budget_complete = built.owner && built.owner->data().upstream_budget_complete;
        inspection->step_regions_published = publish && static_cast<bool>(built.owner);
        inspection->step_regions_operations = built.operations;
        inspection->step_regions_bytes = built.encoded_bytes;
        if (!built.owner)
            inspection->step_regions_gap = "annotation unavailable (reason " +
                std::to_string(static_cast<unsigned>(built.gap)) + ")";
        else {
            inspection->step_regions_gap.reset();
            for (const auto& row : built.owner->data().rows) {
                StepRegionInspection observed;
                observed.function = row.function;
                observed.lookup = row.lookup;
                observed.declaration = module.functions[row.function].declaration;
                observed.site_context = module.functions[row.function];
                observed.round = 1;
                observed.report = *row.report;
                inspection->step_regions.push_back(std::move(observed));
            }
        }
    }
    if (publish) module.step_regions = built.owner;
    if (!built.owner && context.options.diagnostics)
        context.options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableAnalysis,
            {}, "step-region annotation unavailable (reason " +
                std::to_string(static_cast<unsigned>(built.gap)) + ")", {}, {}});
    // This final pass annotates the unchanged instructions. The owned guard
    // includes preceding region annotations, so no later rewrite follows it.
    return false;
}
}
