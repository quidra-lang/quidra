#pragma once

// Tensor regions: the candidate computation regions the optimizer annotates
// on a function (region formation and region candidates).

#include "quidra/ir/value_id.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace quidra::ir {

struct TensorRegionLocation {
    std::size_t block{};
    std::size_t instruction{};
};
struct TensorRegion {
    // Candidate computation region only. It carries no source-visible semantics
    // and does not authorize reordering across explicit placement/tracking/effect
    // boundaries. Later fusion/AD passes may refine a region conservatively.
    std::vector<TensorRegionLocation> instructions;
    std::vector<ValueId> external_inputs;
    std::vector<ValueId> values;
    bool reaches_backward{};
    bool may_require_higher_order{};
    std::vector<std::string> compiler_extensions;
    // Fully qualified descriptor table references (extension:table). Core
    // schedules these generically; package-owned compiler logic interprets
    // their domain semantics.
    std::vector<std::string> compiler_extension_tables;
    // Opaque package operation IDs in region execution order. Core never
    // assigns domain meaning to these IDs.
    std::vector<std::string> compiler_operations;
    // Descriptor-owned fusion tables whose opaque operation sequence matches
    // this region. These are candidates only; package policy owns validity and
    // lowering.
    std::vector<std::string> compiler_fusion_candidates;
};

} // namespace quidra::ir
