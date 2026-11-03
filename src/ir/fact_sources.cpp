#include "ir/fact_sources.hpp"

#include <stdexcept>

namespace quidra::ir::facts {
const FunctionFactSource* FactSources::find(std::string_view name) const noexcept {
    const auto found = functions.find(name);
    return found == functions.end() ? nullptr : &found->second;
}
FactSources adapt_fact_sources(const Module& module, const analysis::SummaryBindings& bindings,
                              const ResultExtentSources& extents) {
    FactSources result;
    for (const auto& function : module.functions) {
        if (function.external_symbol) continue; // Undeclared native return shapes are opaque.
        const auto binding = bindings.find(function.name);
        if (binding == bindings.end()) continue;
        if (binding->second.semantic_parameter_for_ir_parameter.size() != function.parameters.size())
            throw std::invalid_argument("tensor fact source parameter mapping dimensions disagree");
        const auto source = extents.find(binding->second.semantic_key);
        if (source == extents.end()) continue;
        FunctionFactSource captured{source->second, binding->second.semantic_parameter_for_ir_parameter};
        if (!result.functions.emplace(function.name, std::move(captured)).second)
            throw std::invalid_argument("tensor fact source function identity is duplicated");
    }
    return result;
}
} // namespace quidra::ir::facts
