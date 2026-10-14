#include "ir/analysis/step_region_annotations.hpp"
#include "ir/analysis/step_regions.hpp"
#include "ir/fact_sources.hpp"
#include "ir/binding_origins.hpp"
#include "ir/summaries.hpp"
#include "quidra/optimizer.hpp"
#include "optimizer/inspection.hpp"

#include <cstdio>
#include <stdexcept>
#include <type_traits>

using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
Module fixture() {
    const auto natural = Type::simple(TypeKind::Nat64);
    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32), 1);
    Function fn; fn.name = "generic_step"; fn.source_file = "unit.qui"; fn.next_value = 7;
    fn.blocks = {{"entry", {
        SourceLocation{{9, 2}, "unit.qui", "revision", "node", "statement"},
        ConstantInt{1, "4", natural}, ArrayMake{2, {1}, Type::array(natural, 1)},
        TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros},
        DeclareLocal{"saved", tensor, "saved"}, StoreLocal{"saved", 3, tensor},
        LoadLocal{4, "saved", tensor}, TensorBinary{5, "+", 4, 4, tensor, tensor, tensor},
        TensorItem{6, 5, Type::simple(TypeKind::Real32)}, ReturnVoid{}}}};
    Module module; module.functions.push_back(std::move(fn)); return module;
}
void expect_gap(const Module& module, StepRegionAnnotationGap gap) {
    const auto result = read_step_region_annotation(module, 0);
    require(!result.row && !result.owner && result.gap == gap, "changed input retained a usable annotation row");
}
} // namespace
int main() { try {
    auto module = fixture();
    expect_gap(module, StepRegionAnnotationGap::Missing);
    const auto built = build_step_region_annotations(module);
    require(built.owner && built.gap == StepRegionAnnotationGap::None, "actual formation did not produce an owner");
    module.step_regions = built.owner;
    static_assert(std::is_const_v<std::remove_reference_t<decltype(built.owner->data())>>);
    const auto valid = read_step_region_annotation(module, 0);
    require(valid.row && valid.owner == built.owner && valid.row->report &&
        !valid.row->report->regions.empty(), "published table has no real typed region report");
    require(!built.owner->data().upstream_budget_complete, "sidecar invented an upstream budget guarantee");
    auto changed = module;
    std::get<ConstantInt>(changed.functions[0].blocks[0].instructions[1]).value = "5";
    expect_gap(changed, StepRegionAnnotationGap::StaleFunction);
    changed = module; changed.functions[0].next_value++;
    expect_gap(changed, StepRegionAnnotationGap::StaleFunction);
    changed = module; std::get<SourceLocation>(changed.functions[0].blocks[0].instructions[0]).position.column++;
    expect_gap(changed, StepRegionAnnotationGap::StaleFunction);
    changed = module; changed.classes.push_back({"Envelope", {"value"}, {Type::simple(TypeKind::Int64)}, false, {}});
    expect_gap(changed, StepRegionAnnotationGap::StaleEnvironment);
    changed = module; changed.fact_sources = std::make_shared<const facts::FactSources>();
    expect_gap(changed, StepRegionAnnotationGap::StaleEnvironment);
    changed = module; changed.binding_origins = std::make_shared<const BindingOriginSnapshots>();
    expect_gap(changed, StepRegionAnnotationGap::StaleEnvironment);
    changed = module; auto summaries = std::make_shared<ModuleSummaries>();
    changed.summaries = summaries;
    expect_gap(changed, StepRegionAnnotationGap::StaleEnvironment);
    const RegionLocal* saved = nullptr;
    for (const auto& region : valid.row->report->regions)
        for (const auto& local : region.locals) if (local.name == "saved" && local.type && local.type->first) saved = &local;
    require(saved != nullptr, "typed local boundary was not retained");
    auto& source_type = std::get<DeclareLocal>(module.functions[0].blocks[0].instructions[4]).type;
    source_type.first->kind = TypeKind::Real64;
    require(saved->type->first->kind == TypeKind::Real32, "const report still aliases mutable source Type storage");
    expect_gap(module, StepRegionAnnotationGap::StaleFunction);
    source_type.first->kind = TypeKind::Real32;
    // The public Type model contains mutable shared pointees even through a
    // const aggregate. Such a later edit must fail the exact report guard.
    saved->type->first->kind = TypeKind::Real64;
    expect_gap(module, StepRegionAnnotationGap::StaleReport);
    saved->type->first->kind = TypeKind::Real32;
    require(read_step_region_annotation(module, 0).row != nullptr, "exact restored input became unavailable");
    const auto work = build_step_region_annotations(module, {0, 1024});
    require(!work.owner && work.gap == StepRegionAnnotationGap::WorkBudget && module.step_regions == built.owner,
        "work exhaustion published a partial or replaced table");
    const auto bytes = build_step_region_annotations(module, {2'000'000, 0});
    require(!bytes.owner && bytes.gap == StepRegionAnnotationGap::ByteBudget, "byte exhaustion retained partial annotations");
    const auto read_budget = read_step_region_annotation(module, 0, {0, 1024});
    require(!read_budget.row && !read_budget.owner && read_budget.gap == StepRegionAnnotationGap::WorkBudget,
        "reader exhaustion returned a usable row");
    auto duplicate = module; duplicate.functions.push_back(duplicate.functions[0]);
    require(build_step_region_annotations(duplicate).gap == StepRegionAnnotationGap::AmbiguousLookup,
        "ambiguous lookup obtained a source-bound table");
    for (const auto mode : {OptimizerMode::Off, OptimizerMode::Verify, OptimizerMode::On}) {
        optimizer::OptimizerInspection inspection;
        inspection.capture_step_regions = true;
        OptimizerOptions options;
        options.step_regions = mode;
        options.inspection = &inspection;
        const auto optimized = optimize(fixture(), options);
        require(static_cast<bool>(optimized.step_regions) == (mode == OptimizerMode::On),
                "step annotation mode published or skipped an owner incorrectly");
        require(inspection.step_regions_observed == (mode != OptimizerMode::Off),
                "step annotation mode ignored the analysis gate");
        if (mode == OptimizerMode::On)
            require(read_step_region_annotation(optimized, 0).row != nullptr,
                    "pipeline immediately invalidated its final annotation");
    }
    OptimizerOptions verify_cfg;
    verify_cfg.cfg_facts = OptimizerMode::Verify;
    verify_cfg.step_regions = OptimizerMode::On;
    require(!optimize(fixture(), verify_cfg).step_regions,
            "CFG verification published dependent annotations");
    std::puts("step-region annotations: owned report, stale guards, budget and pipeline modes verified");
    return 0;
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; } }
