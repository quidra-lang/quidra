#pragma once

// PassManager: runs the pipeline (pipeline.hpp) on a module. It builds the
// rule registry and function index before the first pass and refreshes them
// after rewrites. A fixpoint group restarts iteratively when a pass changes
// the module. Generated dispatch functions are visible to following passes.

#include "quidra/ir/module.hpp"
#include "quidra/optimizer.hpp"

namespace quidra::optimizer {

class PassManager {
public:
    explicit PassManager(ir::OptimizerOptions options = {}) : options_(options) {}

    ir::Module run(ir::Module module) const;

private:
    ir::OptimizerOptions options_;
};

} // namespace quidra::optimizer
