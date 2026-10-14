#pragma once

#include "ir/summaries.hpp"

namespace quidra::ir::analysis {

// Typed IR effects, propagated to a fixed point across direct recursive calls.
// Semantic path/resource summaries are imported, not inferred a second time.
// Missing/ambiguous/external calls remain opaque until a declared operation
// contract is supplied by the rule registry. Result Fresh refers to actual
// payload origin, never merely allocation of a descriptor or variant carrier.
IrSummaryInputs infer_ir_summary_facts(const Module&, const ModuleSummaries&);

} // namespace quidra::ir::analysis
