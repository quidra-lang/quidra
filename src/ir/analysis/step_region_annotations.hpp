#pragma once

#include "ir/step_region_annotations.hpp"
#include "quidra/ir/module.hpp"

namespace quidra::ir::analysis {
enum class StepRegionAnnotationGap : std::uint8_t {
    None, Missing, InvalidFunction, AmbiguousLookup, InvalidInput,
    InvalidSsa, StaleFunction, StaleEnvironment, StaleReport,
    WorkBudget, ByteBudget, DepthBudget, FormationBudget, Allocation
};
struct StepRegionAnnotationOptions {
    std::uint64_t maximum_operations{2'000'000};
    std::size_t maximum_encoded_bytes{64 * 1024 * 1024};
};
struct StepRegionAnnotationBuild {
    std::shared_ptr<const StepRegionAnnotations> owner;
    StepRegionAnnotationGap gap{StepRegionAnnotationGap::None};
    std::uint64_t operations{};
    std::size_t encoded_bytes{};
};
struct StepRegionAnnotationRead {
    std::shared_ptr<const StepRegionAnnotations> owner;
    const StepRegionAnnotationRow* row{};
    StepRegionAnnotationGap gap{StepRegionAnnotationGap::None};
    std::uint64_t operations{};
    std::size_t encoded_bytes{};
};
// Produces actual reports for the unchanged whole module. The caller may
// publish only owner!=null. Exhaustion/error never returns a partial table.
StepRegionAnnotationBuild build_step_region_annotations(const Module&,
    StepRegionAnnotationOptions = {});
// Valid only for this checked, unchanged input. A later mutation requires a
// fresh read; raw body/dependency/report equality never authorizes motion,
// donation, key-exit lowering or a compiled training-step plan.
StepRegionAnnotationRead read_step_region_annotation(const Module&,
    std::size_t function, StepRegionAnnotationOptions = {});
} // namespace quidra::ir::analysis
