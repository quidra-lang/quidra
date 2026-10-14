#pragma once

#include "optimizer/inspection.hpp"
#include <string>

namespace quidra::cli {
// Versioned diagnostic documents, ordered by function/round/current sites.
// Application and provenance come from owned receipts, never from text or
// source positions. Unknown optional facts remain explicit.
std::string ir_proofs_report(const optimizer::OptimizerInspection&);
std::string ir_step_regions_report(const optimizer::OptimizerInspection&);
} // namespace quidra::cli
