#pragma once

// ir::optimize: the optimizer's pass pipeline over a module (src/optimizer).

#include "quidra/ir/module.hpp"

namespace quidra::ir {

Module optimize(Module module);

} // namespace quidra::ir
