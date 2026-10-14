#include "ir/binding_origins.hpp"
#include "ir/summaries.hpp"

namespace quidra::ir::analysis {
std::string binding_origin_body_fields(const Function& function) {
    auto normalized=function;
    // These explicit non-body fields neither change instruction sites nor
    // storage/formal bindings. Future Function fields remain traversed until
    // classified here; this is not a digest-based semantic equivalence test.
    normalized.name.clear();normalized.c_export_symbol.reset();normalized.next_value=1;
    normalized.tensor_regions.clear();normalized.validated_entry.reset();
    if(normalized.declaration)normalized.declaration->specialization_of.clear();
    return canonical_encode(normalized).bytes;
}
namespace {
bool declaration_matches(const Function& fn,const FunctionBindingSource& source) {
    return fn.declaration && same_declaration(*fn.declaration,source.declaration) &&
        fn.declaration->generic_args==source.declaration.generic_args;
}
}
BindingOriginRead read_binding_origins(const Module& module) {
    BindingOriginRead result;
    const bool class_fields=module.binding_origins &&
        module.binding_origins->class_fields==canonical_encode_value(module.classes).bytes;
    std::map<std::string,std::size_t,std::less<>> counts;
    for(const auto& fn:module.functions)++counts[fn.name];
    for(const auto& fn:module.functions) {
        const auto fail=[&](BindingSourceComponent component,BindingGap reason) {
            result.unavailable.push_back({component,reason,fn.name});
        };
        if(counts.at(fn.name)!=1){fail(BindingSourceComponent::Declaration,BindingGap::Ambiguous);continue;}
        if(!module.binding_origins){fail(BindingSourceComponent::Body,BindingGap::MissingOrigin);continue;}
        if(!class_fields) {
            fail(BindingSourceComponent::ClassLayout,BindingGap::StaleOrigin);continue;
        }
        const auto found=module.binding_origins->functions.find(fn.name);
        if(found==module.binding_origins->functions.end()){fail(BindingSourceComponent::Body,BindingGap::MissingOrigin);continue;}
        const auto& snapshot=found->second;
        if(!declaration_matches(fn,snapshot.source)){fail(BindingSourceComponent::Declaration,BindingGap::MissingOrigin);continue;}
        bool body=false;
        try {body=snapshot.body_fields==binding_origin_body_fields(fn);}
        catch(const std::invalid_argument&) {}
        if(!body){fail(BindingSourceComponent::Body,BindingGap::StaleOrigin);continue;}
        const auto* summary=module.summaries?module.summaries->find(fn.name):nullptr;
        if(!summary || snapshot.source.semantic_parameter_for_ir_parameter!=summary->semantic_parameter_for_ir_parameter ||
           snapshot.source.semantic_parameter_for_ir_parameter.size()!=fn.parameters.size()) {
            fail(BindingSourceComponent::ParameterMap,BindingGap::MissingEffect);continue;
        }
        result.inputs.functions.emplace(fn.name,snapshot.source);
        result.unavailable.insert(result.unavailable.end(),snapshot.unavailable.begin(),snapshot.unavailable.end());
    }
    return result;
}
std::optional<BindingOriginSnapshot> clone_binding_origin(const Function& source,
    const Function& clone,const BindingOriginSnapshot& snapshot,std::vector<BindingSourceIssue>& issues) {
    const auto fail=[&](BindingSourceComponent component,BindingGap gap)->std::optional<BindingOriginSnapshot> {
        issues.push_back({component,gap,clone.name});return {};
    };
    if(!declaration_matches(source,snapshot.source)||!declaration_matches(clone,snapshot.source))
        return fail(BindingSourceComponent::Declaration,BindingGap::MissingOrigin);
    try {
        const auto original=binding_origin_body_fields(source),target=binding_origin_body_fields(clone);
        if(original!=snapshot.body_fields || target!=original)
            return fail(BindingSourceComponent::Body,BindingGap::StaleOrigin);
        auto result=snapshot;result.body_fields=target;
        for(auto& issue:result.unavailable)issue.lookup=clone.name;
        return result;
    }catch(const std::invalid_argument&){return fail(BindingSourceComponent::Body,BindingGap::MissingOrigin);}
}
} // namespace quidra::ir::analysis
