#pragma once

// PassManager: runs the pipeline (pipeline.hpp) on a module. It builds the
// rule registry and the function index once, before the first pass: they
// read the extensions, the function list and each function's name, result
// and source file, which no pass changes. A fixpoint group restarts by a
// recursive call, one level per restart.

#include "quidra/ir/module.hpp"

namespace quidra::optimizer {

class PassManager {
public:
    ir::Module run(ir::Module module) const;
};

} // namespace quidra::optimizer
