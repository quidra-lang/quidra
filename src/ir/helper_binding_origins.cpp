#include "ir/helper_binding_origins.hpp"
#include "ir/analysis/tensor_facts.hpp"
#include "ir/analysis/cfg.hpp"
#include "ir/analysis/dominators.hpp"
#include "ir/analysis/local_ssa.hpp"
#include "ir/summaries.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace quidra::ir::analysis {
namespace {
struct Missing : std::runtime_error {using std::runtime_error::runtime_error;};
std::string abi_fields(Function fn){fn.blocks.clear();return binding_origin_body_fields(fn);}
bool identity(const Function& source,const Function& clone) {
    return source.declaration && clone.declaration && same_declaration(*source.declaration,*clone.declaration) &&
        source.declaration->generic_args==clone.declaration->generic_args && abi_fields(source)==abi_fields(clone);
}
const Function* unique(std::span<const Function> functions,std::string_view name) {
    const Function* result=nullptr;
    for(const auto& fn:functions)if(fn.name==name){if(result)return nullptr;result=&fn;}
    return result;
}
bool dirty(const Module& module,const Instruction& ins) {
    if(const auto* call=std::get_if<Call>(&ins))return !helper_descriptor_read_only(module,*call);
    return std::holds_alternative<IndirectCall>(ins)||std::holds_alternative<ParallelBlock>(ins)||
        std::holds_alternative<StoreAddress>(ins)||std::holds_alternative<StoreReference>(ins)||
        std::holds_alternative<FieldSet>(ins)||std::holds_alternative<ArraySet>(ins)||
        std::holds_alternative<ArraySliceSet>(ins)||std::holds_alternative<TensorSet>(ins)||std::holds_alternative<TensorBackward>(ins);
}
const BindingOriginSnapshot* fresh(const Module& module,const Function& fn,const BindingOriginRead& read) {
    if(!module.binding_origins)return nullptr;
    const auto row=module.binding_origins->functions.find(fn.name);
    if(row==module.binding_origins->functions.end() || row->second.body_fields!=binding_origin_body_fields(fn))return nullptr;
    if(row->second.root_source_available && !read.inputs.functions.contains(fn.name))return nullptr;
    return &row->second;
}
bool entry_matches(const Function& source,const facts::ValidatedEntryFacts& entry,std::string_view source_instance) {
    return source.declaration && entry.origin==facts::EntryFactOrigin::SpecializedCalls &&
        !entry.fact_key.empty() && !source_instance.empty() && entry.source_instance==source_instance &&
        same_declaration(*source.declaration,entry.declaration) &&
        source.declaration->generic_args==entry.declaration.generic_args;
}
bool prior_source_link(const Module& module,const Function& source,const Function& clone,
    const BindingOriginRead& read,const HelperCallRetargetReceipt& receipt) {
    if(!identity(source,clone) || !fresh(module,clone,read) || !module.summaries ||
       !receipt.clone_entry || clone.validated_entry!=receipt.clone_entry ||
       !entry_matches(source,*receipt.clone_entry,receipt.source_instance) ||
       clone.declaration->specialization_of!=declaration_key(*source.declaration))return false;
    const auto* a=module.summaries->find(source.name);const auto* b=module.summaries->find(clone.name);
    if(!a || !b || !a->semantic || b->semantic!=a->semantic || b->semantic_key!=a->semantic_key ||
       b->semantic_parameter_for_ir_parameter!=a->semantic_parameter_for_ir_parameter)return false;
    const auto owned=module.summaries->semantic_snapshots().find(a->semantic_key);
    return owned!=module.summaries->semantic_snapshots().end() && &owned->second==a->semantic;
}
} // namespace
bool helper_descriptor_read_only(const Module& module,const Call& call) {
    if(!module.summaries)return false;
    const auto* summary=module.summaries->find(call.callee);
    if(!summary || !summary->semantic || !summary->descriptor_complete || summary->descriptor_effects ||
       !summary->effects_complete || (summary->effects&effect_bit::Opaque))return false;
    const auto& effect=*summary->semantic;
    return !effect.mutates_ambient && !effect.mutates_receiver && effect.reference_shared_writes.empty() &&
        effect.reference_shared_write_origins.empty() &&
        std::none_of(effect.mutates_shared_of_parameter.begin(),effect.mutates_shared_of_parameter.end(),[](bool value){return value;}) &&
        std::none_of(effect.exposed_writes.begin(),effect.exposed_writes.end(),[](const auto& value){return !value.empty();});
}
std::shared_ptr<const HelperTrackingProof> prove_helper_tracking_folds(const Module& module,
    const Function& fn,std::shared_ptr<const facts::ValidatedEntryFacts> entry,const std::function<void()>& tick) {
    auto proof=std::make_shared<HelperTrackingProof>();proof->body_=binding_origin_body_fields(fn);proof->entry_=std::move(entry);
    if(!proof->entry_ || !fn.declaration || proof->entry_->origin!=facts::EntryFactOrigin::SpecializedCalls ||
       proof->entry_->source_instance.empty() || proof->entry_->fact_key.empty() ||
       !same_declaration(*fn.declaration,proof->entry_->declaration) || fn.declaration->generic_args!=proof->entry_->declaration.generic_args)
        throw std::invalid_argument("tracking fold lacks an exact owned specialized entry");
    for(const auto& [index,tensor]:proof->entry_->tensors) {
        if(index>=fn.parameters.size() || fn.parameters[index].type.kind!=TypeKind::Tensor ||
           (tensor.has_type && (!tensor.type.first || !fn.parameters[index].type.first || *tensor.type.first!=*fn.parameters[index].type.first)) ||
           tensor.owns_storage || tensor.unique!=facts::Tri::Unknown || tensor.fully_initialized!=facts::Tri::Unknown)
            throw std::invalid_argument("tracking entry does not match the concrete formal contract");
    }
    const auto cfg=build_cfg(fn);const auto dom=dominators(cfg);const auto slots=classify_slots(fn);const auto ssa=build_local_ssa(fn,cfg,dom,slots);
    const ModuleSummaries no_summaries;const facts::FactSources no_sources;
    const ParamFacts parameters{proof->entry_->tensors,proof->entry_->scalars,proof->entry_->policies};
    const auto flow=compute_tensor_facts(fn,cfg,dom,ssa,slots,module.summaries?*module.summaries:no_summaries,
        module.fact_sources?*module.fact_sources:no_sources,parameters);
    std::vector<bool> incoming(cfg.succ.size());bool changed=true;
    while(changed){changed=false;for(const auto block:cfg.rpo){tick();bool marked=incoming[block];
        for(const auto& ins:fn.blocks[block].instructions)marked|=dirty(module,ins);
        if(marked)for(const auto successor:cfg.succ[block])if(!incoming[successor]){incoming[successor]=true;changed=true;}
    }}
    for(const auto block:cfg.rpo){bool marked=incoming[block];
        for(std::uint32_t index=0;index<fn.blocks[block].instructions.size();++index){tick();const auto& ins=fn.blocks[block].instructions[index];
            if(const auto* query=std::get_if<TensorIsTracked>(&ins);query && !marked && flow.at({block,index},query->tensor).tracked==facts::Tri::False)
                proof->replacements_.push_back({{block,index},query->out,query->tensor});
            marked|=dirty(module,ins);
        }
    }
    return proof;
}
bool helper_unchanged_body(const Function& source,const Function& clone) {
    try{return identity(source,clone) && binding_origin_body_fields(source)==binding_origin_body_fields(clone);}
    catch(const std::invalid_argument&){return false;}
}
HelperOriginResult prepare_helper_origins(const Module& module,std::span<const Function> clones,
    std::span<const HelperCloneOriginReceipt> clone_receipts,std::span<const HelperCallRetargetReceipt> retargets) {
    HelperOriginResult result;if(!module.binding_origins)return result;
    auto snapshots=std::make_shared<BindingOriginSnapshots>(*module.binding_origins);
    const auto read=read_binding_origins(module);
    if(snapshots->class_fields!=canonical_encode_value(module.classes).bytes)return {module.binding_origins,{{BindingSourceComponent::ClassLayout,BindingGap::StaleOrigin,{}}}};
    std::map<std::string,const HelperCloneOriginReceipt*,std::less<>> sources;
    const auto unavailable=[&](std::string function,BindingSourceComponent component,BindingGap reason){result.unavailable.push_back({component,reason,std::move(function)});};
    for(const auto& receipt:clone_receipts) {
        const auto* source=unique(module.functions,receipt.source);const auto* clone=unique(clones,receipt.clone);
        try {
            if(!source || !clone || snapshots->functions.contains(receipt.clone) || !receipt.proof || !identity(*source,*clone) ||
               receipt.proof->body_fields()!=binding_origin_body_fields(*source) ||
               clone->declaration->specialization_of!=declaration_key(*source->declaration))throw Missing("helper clone preimage/ABI is unavailable");
            const auto* origin=fresh(module,*source,read);if(!origin || !origin->root_source_available)throw Missing("helper source row is missing");
            auto expected=*source;expected.name=clone->name;expected.c_export_symbol.reset();expected.entrypoint=false;
            expected.declaration->specialization_of=clone->declaration->specialization_of;
            std::vector<BindingSourceIssue> issues;
            auto output=clone_binding_origin(*source,expected,*origin,issues);
            if(!output)throw Missing("helper original clone has changed fields");
            std::set<InstrRef,InlineSiteLess> sites;
            for(const auto& replacement:receipt.proof->replacements()) {
                if(!sites.insert(replacement.at).second || replacement.at.block>=expected.blocks.size() ||
                   replacement.at.index>=expected.blocks[replacement.at.block].instructions.size())throw Missing("tracking replacement site is invalid");
                auto& ins=expected.blocks[replacement.at.block].instructions[replacement.at.index];
                const auto* query=std::get_if<TensorIsTracked>(&ins);
                if(!query || query->out!=replacement.out || query->tensor!=replacement.tensor)throw Missing("tracking replacement is not the original query");
                ins=ConstantBool{replacement.out,false};
            }
            if(binding_origin_body_fields(expected)!=binding_origin_body_fields(*clone))throw Missing("helper changed an unproven instruction field");
            output->body_fields=binding_origin_body_fields(*clone);
            snapshots->functions.emplace(receipt.clone,std::move(*output));sources.emplace(receipt.clone,&receipt);
        }catch(const Missing&){unavailable(receipt.clone,BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);}
        catch(const std::invalid_argument&){unavailable(receipt.clone,BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);}
    }
    std::map<std::string,Function,std::less<>> callers;
    std::map<std::string,std::shared_ptr<const std::string>,std::less<>> caller_preimages;
    std::set<std::pair<std::string,InstrRef>,bool(*)(const std::pair<std::string,InstrRef>&,const std::pair<std::string,InstrRef>&)> sites(
        [](const auto& a,const auto& b){if(a.first!=b.first)return a.first<b.first;return InlineSiteLess{}(a.second,b.second);});
    std::set<std::string,std::less<>> bad;
    for(const auto& receipt:retargets) {
        try {
            const auto* caller=unique(module.functions,receipt.caller);const auto* source=unique(module.functions,receipt.source);
            const auto* clone=unique(clones,receipt.clone);if(!clone)clone=unique(module.functions,receipt.clone);
            if(!caller || !source || !clone || !receipt.caller_body ||
               !sites.emplace(receipt.caller,receipt.at).second || receipt.at.block>=caller->blocks.size() ||
               receipt.at.index>=caller->blocks[receipt.at.block].instructions.size())throw Missing("retarget caller preimage is invalid");
            const auto prior_body=caller_preimages.find(caller->name);
            if(prior_body==caller_preimages.end()) {
                if(*receipt.caller_body!=binding_origin_body_fields(*caller))throw Missing("retarget caller BODY has changed");
                caller_preimages.emplace(caller->name,receipt.caller_body);
            } else if(prior_body->second!=receipt.caller_body && *prior_body->second!=*receipt.caller_body)
                throw Missing("retarget caller BODY receipt is inconsistent");
            const auto* old=std::get_if<Call>(&caller->blocks[receipt.at.block].instructions[receipt.at.index]);
            if(!old || old->callee!=source->name || canonical_encode_value(*old).bytes!=canonical_encode_value(receipt.before).bytes ||
               canonical_location_bytes(canonical_encode_value(*old).locations)!=canonical_location_bytes(canonical_encode_value(receipt.before).locations))
                throw Missing("retarget old call is not the actual instruction");
            auto expected=*old;expected.callee=clone->name;
            if(canonical_encode_value(expected).bytes!=canonical_encode_value(receipt.after).bytes ||
               canonical_location_bytes(canonical_encode_value(expected).locations)!=canonical_location_bytes(canonical_encode_value(receipt.after).locations) ||
               !identity(*source,*clone))throw Missing("retarget changed actual call/ABI fields");
            const auto created=sources.find(clone->name);
            const bool new_link=created!=sources.end() && created->second->source==source->name &&
                receipt.clone_entry && created->second->proof->entry()==receipt.clone_entry &&
                entry_matches(*source,*receipt.clone_entry,receipt.source_instance);
            if(!new_link && !prior_source_link(module,*source,*clone,read,receipt))
                throw Missing("retarget lacks an owned explicit clone-source link");
            auto [current,inserted]=callers.try_emplace(caller->name,*caller);(void)inserted;
            current->second.blocks[receipt.at.block].instructions[receipt.at.index]=receipt.after;
        }catch(const Missing&){bad.insert(receipt.caller);unavailable(receipt.caller,BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);}
        catch(const std::invalid_argument&){bad.insert(receipt.caller);unavailable(receipt.caller,BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);}
    }
    for(auto& [name,fn]:callers) {
        if(bad.contains(name)){snapshots->functions.erase(name);continue;}
        const auto* original=unique(module.functions,name);const auto* origin=original?fresh(module,*original,read):nullptr;
        BindingOriginSnapshot output;
        if(origin)output=*origin;
        else {output.root_source_available=false;output.unavailable.push_back({BindingSourceComponent::Body,BindingGap::MissingOrigin,name});}
        // Exact call retargets preserve every instruction boundary, SSA/local,
        // scope and semantic path. Only the authorized callee field changed.
        output.body_fields=binding_origin_body_fields(fn);snapshots->functions.insert_or_assign(name,std::move(output));
    }
    for(const auto& name:bad)snapshots->functions.erase(name);
    result.snapshots=std::move(snapshots);return result;
}
} // namespace quidra::ir::analysis
