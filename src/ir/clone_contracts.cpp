#include "ir/clone_contracts.hpp"
#include "ir/canonical_encoding.hpp"

#include <map>

namespace quidra::ir::analysis {
// The only private plumbing: retain the immutable semantic owner without
// recopying checker snapshots or exposing a mutable semantic table.
struct CloneContractAccess {
    static ModuleSummaries share(const ModuleSummaries* source) {
        ModuleSummaries result;
        if (source) {
            result.semantic_ = source->semantic_;
            result.by_function = source->by_function;
        }
        return result;
    }
};
} // namespace quidra::ir::analysis

namespace quidra::ir {
namespace {
void normalize_origin(std::optional<DeclarationIdentity>& declaration) {
    if (declaration) declaration->specialization_of.clear();
}
bool same_abi(const Function& source, const Function& clone) {
    struct Abi {
        std::vector<Parameter> parameters;
        Type result;
        bool entrypoint{}, shape_wrapper{}, dispatch_stub{};
        std::optional<ParallelNodeMetadata> parallel_node;
        std::optional<std::string> external_symbol;
        std::string source_function;
        std::optional<DeclarationIdentity> declaration;
    };
    const auto capture = [](const Function& function) {
        Abi result{function.parameters,function.result,function.entrypoint,function.shape_wrapper,function.dispatch_stub,
            function.parallel_node,function.external_symbol,function.source_function,function.declaration};
        normalize_origin(result.declaration);
        return canonical_encode_value(result).bytes;
    };
    return capture(source) == capture(clone);
}
bool unchanged_body(const Function& source, const Function& clone) {
    const auto capture = [](Function function) {
        // These fields are permitted clone provenance/export/allocation
        // differences. Every appended executable field is still traversed.
        function.name.clear();
        function.c_export_symbol.reset();
        function.next_value = 1;
        normalize_origin(function.declaration);
        return canonical_encode(function).bytes;
    };
    return capture(source) == capture(clone);
}
bool semantic_binding_valid(const analysis::ModuleSummaries& owner,
                            const analysis::FunctionSummary& source, const Function& function) {
    if (!source.semantic || source.semantic_key.empty()) return false;
    const auto owned = owner.semantic_snapshots().find(source.semantic_key);
    if (owned == owner.semantic_snapshots().end() || &owned->second != source.semantic) return false;
    if (source.semantic_parameter_for_ir_parameter.size() != function.parameters.size()) return false;
    for (const auto parameter : source.semantic_parameter_for_ir_parameter)
        if (parameter && *parameter >= source.semantic->exposed_reads.size()) return false;
    return true;
}
} // namespace

CloneContractResult bind_clone_contracts(const Module& module, std::span<const CloneReference> clones,
                                        std::span<const SynthesizedFunctionReference> synthesized) {
    CloneContractResult result;
    auto summaries = analysis::CloneContractAccess::share(module.summaries.get());
    auto sources = module.fact_sources ? *module.fact_sources : facts::FactSources{};
    std::map<std::string,std::vector<const Function*>,std::less<>> functions;
    for (const auto& function : module.functions) functions[function.name].push_back(&function);
    std::erase_if(summaries.by_function,[&](const auto& row) { return !functions.contains(row.first); });
    std::map<std::string,std::size_t,std::less<>> targets;
    for (const auto& clone : clones) ++targets[clone.clone];
    for (const auto& generated : synthesized) ++targets[generated.function];
    const auto issue = [&](CloneContractStatus& status, CloneContractComponent component, CloneUnavailable reason) {
        result.unavailable.push_back({component,reason,status.function,status.source});
    };
    const auto find = [&](std::string_view name, CloneContractStatus& status) -> const Function* {
        const auto row = functions.find(name);
        if (row == functions.end()) { issue(status,CloneContractComponent::Reference,CloneUnavailable::MissingFunction); return nullptr; }
        if (row->second.size() != 1) { issue(status,CloneContractComponent::Reference,CloneUnavailable::AmbiguousFunction); return nullptr; }
        return row->second[0];
    };
    const auto destination = [&](CloneContractStatus& status) -> const Function* {
        if (status.function.empty() || (!status.synthesized && (status.source.empty() || status.function == status.source))) {
            issue(status,CloneContractComponent::Reference,CloneUnavailable::InvalidReference); return nullptr;
        }
        const auto* function = find(status.function,status);
        if (!function) return nullptr;
        if (targets.at(status.function) != 1) {
            issue(status,CloneContractComponent::Reference,CloneUnavailable::DuplicateReference); return nullptr;
        }
        if (summaries.by_function.contains(status.function) || sources.functions.contains(status.function) ||
            sources.policies.setters.contains(status.function) || sources.policies.writes_policy.contains(status.function)) {
            issue(status,CloneContractComponent::Reference,CloneUnavailable::DestinationExists); return nullptr;
        }
        return function;
    };
    // Authorized targets get an opaque row even when their supplied source
    // is unavailable; this enables safe refresh without concealing the issue.
    const auto conservative = [&](const CloneContractStatus& status) {
        if (const auto row = functions.find(status.function); row != functions.end() && row->second.size() == 1)
            summaries.by_function.try_emplace(status.function,analysis::FunctionSummary{});
    };
    for (const auto& reference : clones) {
        CloneContractStatus status;
        status.function = reference.clone; status.source = reference.source;
        const auto* clone = destination(status);
        if (!clone) { conservative(status); result.bindings.push_back(std::move(status)); continue; }
        const auto* source = find(reference.source,status);
        if (!source) { conservative(status); result.bindings.push_back(std::move(status)); continue; }
        bool abi = false;
        try { abi = same_abi(*source,*clone); }
        catch (const std::invalid_argument&) { /* Unencodable ABI remains unavailable. */ }
        if (!abi) {
            issue(status,CloneContractComponent::Reference,CloneUnavailable::AbiMismatch);
            conservative(status); result.bindings.push_back(std::move(status)); continue;
        }
        status.reference_available = true;
        const auto* summary = module.summaries ? module.summaries->find(reference.source) : nullptr;
        if (!summary) issue(status,CloneContractComponent::Semantic,CloneUnavailable::MissingSourceSummary);
        else if (summary->semantic_parameter_for_ir_parameter.size() != source->parameters.size())
            issue(status,CloneContractComponent::Semantic,CloneUnavailable::FormalMappingMismatch);
        else if (!semantic_binding_valid(*module.summaries,*summary,*source))
            issue(status,CloneContractComponent::Semantic,CloneUnavailable::MissingSemanticSnapshot);
        else {
            auto bound = *summary;
            static_cast<analysis::IrSummaryFacts&>(bound) = {};
            bound.pure_tensor = false;
            status.semantic_available = true;
            if (reference.ir_proofs == CloneIrProofs::PreserveUnchangedBody) {
                try {
                    if (unchanged_body(*source,*clone)) {
                        static_cast<analysis::IrSummaryFacts&>(bound) = *summary;
                        bound.pure_tensor = summary->pure_tensor;
                        status.ir_proofs_preserved = true;
                    } else issue(status,CloneContractComponent::IrProofs,CloneUnavailable::ChangedBody);
                } catch (const std::invalid_argument&) {
                    issue(status,CloneContractComponent::IrProofs,CloneUnavailable::UncomparableBody);
                }
            }
            summaries.by_function.emplace(reference.clone,std::move(bound));
        }
        if (!status.semantic_available) conservative(status);
        const auto* fact = module.fact_sources ? module.fact_sources->find(reference.source) : nullptr;
        if (!fact) issue(status,CloneContractComponent::TensorFacts,CloneUnavailable::MissingSourceFacts);
        else if (!status.semantic_available || fact->semantic_parameter_for_ir_parameter !=
                 summary->semantic_parameter_for_ir_parameter)
            issue(status,CloneContractComponent::TensorFacts,CloneUnavailable::FormalMappingMismatch);
        else if (sources.functions.contains(reference.clone))
            issue(status,CloneContractComponent::TensorFacts,CloneUnavailable::DestinationExists);
        else {
            sources.functions.emplace(reference.clone,*fact);
            status.result_extents_available = true;
        }
        if (module.fact_sources) {
            const auto& policies = module.fact_sources->policies;
            const auto setter = policies.setters.find(reference.source);
            const auto write = policies.writes_policy.find(reference.source);
            status.policy_setter = setter == policies.setters.end() ? ClonePolicyAvailability::NotDeclared : ClonePolicyAvailability::Present;
            status.policy_write = write == policies.writes_policy.end() ? ClonePolicyAvailability::NotDeclared : ClonePolicyAvailability::Present;
            if (sources.policies.setters.contains(reference.clone) || sources.policies.writes_policy.contains(reference.clone)) {
                status.policy_setter = status.policy_write = ClonePolicyAvailability::Unavailable;
                issue(status,CloneContractComponent::Policy,CloneUnavailable::DestinationExists);
            } else {
                if (setter != policies.setters.end()) sources.policies.setters.emplace(reference.clone,setter->second);
                if (write != policies.writes_policy.end()) sources.policies.writes_policy.emplace(reference.clone,write->second);
            }
        } else issue(status,CloneContractComponent::Policy,CloneUnavailable::MissingSourceFacts);
        result.bindings.push_back(std::move(status));
    }
    for (const auto& reference : synthesized) {
        CloneContractStatus status;
        status.function = reference.function; status.synthesized = true;
        if (destination(status)) status.reference_available = true;
        conservative(status);
        result.bindings.push_back(std::move(status));
    }
    for (const auto& [name,rows] : functions) {
        if (!summaries.by_function.contains(name))
            result.unavailable.push_back({CloneContractComponent::Semantic,CloneUnavailable::InitialSummaryOmission,name,{}});
        if (rows.size() != 1)
            result.unavailable.push_back({CloneContractComponent::Reference,CloneUnavailable::AmbiguousFunction,name,{}});
    }
    result.summaries = std::make_shared<const analysis::ModuleSummaries>(std::move(summaries));
    result.fact_sources = std::make_shared<const facts::FactSources>(std::move(sources));
    return result;
}
} // namespace quidra::ir
