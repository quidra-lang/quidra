#include "ir/summaries.hpp"

#include <algorithm>
#include <stdexcept>

namespace quidra::ir::analysis {
namespace {
void validate_facts(const Function& function, const IrSummaryFacts& summary) {
    if (summary.result == ResultAlias::ParamDerived &&
        (!summary.result_parameter || *summary.result_parameter >= function.parameters.size()))
        throw std::invalid_argument("IR summary result parameter is out of range");
    if (summary.result != ResultAlias::ParamDerived && summary.result_parameter)
        throw std::invalid_argument("IR summary result parameter lacks parameter-derived provenance");
}
void apply_contract(const Function& function, FunctionSummary& summary) {
    validate_facts(function, summary);
    const bool opaque = function.external_symbol.has_value() || !summary.semantic;
    if (summary.semantic && !summary.semantic->resources.empty()) summary.effects |= effect_bit::ExternalIO;
    if (opaque || !summary.effects_complete || (summary.effects & effect_bit::Opaque)) {
        summary.effects |= effect_bit::Opaque;
        summary.effects_complete = false;
    }
    if (opaque) {
        summary.result = ResultAlias::Unknown;
        summary.result_parameter.reset();
        summary.result_complete = false;
        summary.descriptor_complete = false;
    }
    const auto writes = summary.semantic &&
        (summary.semantic->mutates_receiver || summary.semantic->mutates_ambient ||
         std::any_of(summary.semantic->mutates_shared_of_parameter.begin(), summary.semantic->mutates_shared_of_parameter.end(), [](bool value) { return value; }) ||
         std::any_of(summary.semantic->exposed_writes.begin(), summary.semantic->exposed_writes.end(), [](const auto& paths) { return !paths.empty(); }) ||
         !summary.semantic->reference_shared_writes.empty() || !summary.semantic->reference_shared_write_origins.empty());
    summary.pure_tensor = summary.semantic && summary.effects_complete && summary.descriptor_complete &&
        !summary.descriptor_effects && !writes &&
        !(summary.effects & (effect_bit::HostSync | effect_bit::ExternalIO | effect_bit::WritesPolicy | effect_bit::Opaque));
}
} // namespace

const FunctionSummary* ModuleSummaries::find(std::string_view name) const noexcept {
    const auto found = by_function.find(name);
    return found == by_function.end() ? nullptr : &found->second;
}
const ModuleSummaries::SemanticSnapshots& ModuleSummaries::semantic_snapshots() const noexcept {
    static const SemanticSnapshots empty;
    return semantic_ ? *semantic_ : empty;
}
ModuleSummaries adapt_summaries(const Module& module, const semantics::EffectSummaries& checked,
                               const SummaryBindings& bindings) {
    ModuleSummaries result;
    auto snapshots = std::make_shared<ModuleSummaries::SemanticSnapshots>();
    for (const auto& [key, summary] : checked.all()) snapshots->emplace(key, summary);
    result.semantic_ = std::move(snapshots);
    for (const auto& function : module.functions) {
        FunctionSummary summary;
        if (const auto binding = bindings.find(function.name); binding != bindings.end()) {
            summary.semantic_key = binding->second.semantic_key;
            summary.semantic_parameter_for_ir_parameter = binding->second.semantic_parameter_for_ir_parameter;
            if (summary.semantic_parameter_for_ir_parameter.size() != function.parameters.size())
                throw std::invalid_argument("IR summary parameter mapping dimensions disagree");
            const auto semantic = result.semantic_->find(summary.semantic_key);
            if (semantic != result.semantic_->end()) {
                summary.semantic = &semantic->second;
                for (const auto index : summary.semantic_parameter_for_ir_parameter)
                    if (index && *index >= summary.semantic->exposed_reads.size())
                        throw std::invalid_argument("IR summary semantic parameter is out of range");
            }
        }
        apply_contract(function, summary);
        if (!result.by_function.emplace(function.name, std::move(summary)).second)
            throw std::invalid_argument("IR summary function identity is duplicated");
    }
    return result;
}
ModuleSummaries apply_ir_summary_facts(const Module& module, const ModuleSummaries& imported,
                                      const IrSummaryInputs& inputs) {
    ModuleSummaries result;
    result.semantic_ = imported.semantic_;
    for (const auto& function : module.functions) {
        const auto* source = imported.find(function.name);
        if (!source) throw std::invalid_argument("IR summary import lacks the function identity");
        auto summary = *source;
        // Facts are supplied by IR analyses. This metadata adapter never
        // traverses a body or infers a callee/CFG/storage effect itself.
        static_cast<IrSummaryFacts&>(summary) = {};
        if (const auto facts = inputs.find(function.name); facts != inputs.end())
            static_cast<IrSummaryFacts&>(summary) = facts->second;
        if ((!summary.semantic_key.empty() || !summary.semantic_parameter_for_ir_parameter.empty()) &&
            summary.semantic_parameter_for_ir_parameter.size() != function.parameters.size())
            throw std::invalid_argument("IR summary enriched parameter mapping dimensions disagree");
        apply_contract(function, summary);
        if (!result.by_function.emplace(function.name, std::move(summary)).second)
            throw std::invalid_argument("IR summary function identity is duplicated");
    }
    return result;
}
} // namespace quidra::ir::analysis
