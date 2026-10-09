#pragma once

// ChainFusionPass: replaces a chain of package operation calls with the call
// of the operation a fusion pattern names as its replacement.
//
// A chain is a run of calls in one block, each implementing the next
// operation of the pattern and consuming the previous call's result; only
// pure bridges (source locations, loads, field reads, clones, retains) and
// releases of the chain's values may sit between the calls. The
// replacement takes the later calls' side inputs, then the first call's
// arguments, and is emitted at the last call, so side inputs are evaluated
// in their original order; writable side inputs are refused. The
// replacement must keep every safety trait of the chain's operations, take
// its arguments as they were taken, return a compatible result, and be the
// only function of its operation that does. Patterns are tried longest
// first, so a shorter prefix never takes a chain a longer pattern matches.

#include "optimizer/pass_context.hpp"

#include "quidra/ir/module.hpp"

namespace quidra::optimizer {

class ChainFusionPass {
public:
    // The pass: fuses chains until none is left; true when it fused any.
    static bool run(ir::Module& module, const PassContext& context);
};

} // namespace quidra::optimizer
