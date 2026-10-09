#pragma once

// PassContext: what the passes of the optimizer read besides the module they
// rewrite, the rule registry and the function index of that module. Both
// are built before the first pass and outlive the last one. The rewrites
// change blocks and the region passes each function's regions; no pass
// changes the function list or the extensions, so both stay valid while
// the passes run.

#include "optimizer/function_index.hpp"
#include "optimizer/rule_registry.hpp"

namespace quidra::optimizer {

struct PassContext {
    const RuleRegistry& registry;
    const FunctionIndex& functions;
};

} // namespace quidra::optimizer
