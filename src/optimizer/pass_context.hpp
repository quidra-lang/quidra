#pragma once

// PassContext: what the passes of the optimizer read besides the module they
// rewrite: the rule registry and the function index of that module, and the
// optimizer's switches (ir::OptimizerOptions). The registry and the index
// outlive the passes. The manager rebuilds them after rewrites change call
// contracts or append functions, before another pass can observe those changes.

#include "optimizer/function_index.hpp"
#include "optimizer/rule_registry.hpp"
#include "quidra/optimizer.hpp"

namespace quidra::ir::analysis { class AnalysisCache; }

namespace quidra::optimizer {

struct PassContext {
    const RuleRegistry& registry;
    const FunctionIndex& functions;
    const ir::OptimizerOptions& options;
    ir::analysis::AnalysisCache* analyses{};
};

} // namespace quidra::optimizer
