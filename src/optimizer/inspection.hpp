#pragma once

// Owned diagnostic receipts from the actual optimizer passes. Input-site
// contexts retain per-round instruction references after later rewrites;
// this model does not enable a pass or authorize a transformation.
#include "optimizer/check_elimination.hpp"
#include "ir/analysis/step_regions.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::optimizer {
enum class InspectionOrigin : std::uint8_t { Pass, Collector };
struct ProofInspection {
    std::size_t function{};
    std::string lookup;
    std::optional<DeclarationIdentity> declaration;
    std::uint64_t round{};
    ir::Function site_context;
    InspectionOrigin origin{InspectionOrigin::Pass};
    bool ready{};
    CheckEliminationReport report;
    bool application_observed{};
    std::optional<CheckApplyResult> application;
};
struct StepRegionInspection {
    std::size_t function{};
    std::string lookup;
    std::optional<DeclarationIdentity> declaration;
    std::uint64_t round{};
    ir::Function site_context;
    InspectionOrigin origin{InspectionOrigin::Pass};
    ir::analysis::StepRegionReport report;
};
struct OptimizerInspection {
    bool capture_proofs{}, capture_step_regions{};
    bool proofs_observed{}, step_regions_observed{};
    bool step_regions_ready{}, step_regions_published{};
    bool step_regions_upstream_budget_complete{};
    std::optional<std::string> step_regions_gap;
    std::uint64_t step_regions_operations{}, step_regions_bytes{};
    std::vector<ProofInspection> proofs;
    std::vector<StepRegionInspection> step_regions;
};
} // namespace quidra::optimizer
