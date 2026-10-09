#pragma once

// DataflowComponentRegions: the tensor regions of each function, formed as
// the connected components of its tensor producer-consumer graph. A node is
// a tensor operation, or a call of a package operation that returns a tensor
// and takes no writable argument; an edge joins a node that produces a value
// to one that consumes it. A component with two or more nodes, or with a
// package operation, becomes a region (ir::TensorRegion), which also lists
// the extensions of the package that holds the function. Regions are
// metadata: no rewrite reads them.
//
// Region formation v2 is a separate, later pass beside this one.

#include "optimizer/pass_context.hpp"

#include "quidra/ir/module.hpp"

namespace quidra::optimizer {

class DataflowComponentRegions {
public:
    // The pass: forms the regions of every function of module, then adds to
    // each region the tensor-region extensions whose package holds the
    // function. False: it changes no instruction.
    static bool run(ir::Module& module, const PassContext& context);
};

} // namespace quidra::optimizer
