#pragma once

// RegionCandidateAnnotation: annotates each tensor region with the
// descriptor tables of its extensions (compiler_extension_tables) and with
// the fusion patterns whose operation chain occurs in it, each call
// consuming the previous one's result (compiler_fusion_candidates). The
// patterns are candidates that package policy decides on; no rewrite reads
// the annotations.

#include "optimizer/pass_context.hpp"

#include "quidra/ir/module.hpp"

namespace quidra::optimizer {

class RegionCandidateAnnotation {
public:
    // The pass: annotates every region of module. False: it changes no
    // instruction.
    static bool run(ir::Module& module, const PassContext& context);
};

} // namespace quidra::optimizer
