#include "ir/analysis/loop_invariants.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace quidra::ir::analysis {
namespace {
using Site=std::pair<BlockId,std::uint32_t>;
struct Reads:OperandVisitor {
    std::vector<ValueId> values;bool available{true};
    void use(ValueId value,OperandUse role) override {
        if(value)values.push_back(value);
        available &= role.role_available && role.role!=OperandRole::Address && role.role!=OperandRole::AutogradTarget;
    }
};
const Instruction* definition(const Function& fn,const DefUse& uses,ValueId value) {
    const auto* record=uses.find(value);if(!record || record->definitions.size()!=1)return nullptr;
    const auto at=record->definitions.front();
    return at.block<fn.blocks.size() && at.index<fn.blocks[at.block].instructions.size()?&fn.blocks[at.block].instructions[at.index]:nullptr;
}
bool scalar_type(const Type& type) {return is_numeric(type) || type.kind==TypeKind::Bool || type.kind==TypeKind::String || type.kind==TypeKind::Function;}
void reason(std::vector<InvariantReason>& reasons,InvariantReason value) {
    if(std::find(reasons.begin(),reasons.end(),value)==reasons.end())reasons.push_back(value);
}
struct Writes {
    bool unknown{}, policy{}, mutable_aggregates{};
    std::map<std::string,std::vector<InstrRef>,std::less<>> locals;
    std::map<StorageClassId,std::vector<InstrRef>> payload,descriptor;
    std::set<StorageClassId> other_descriptor;
    std::map<StorageClassId,std::vector<InstrRef>> created;
    std::vector<InvariantReason> reasons;
};
void storage_write(Writes& writes,const AliasInfo& aliases,ValueId value,InstrRef at,bool payload,bool descriptor,bool other=true) {
    const auto* storage=aliases.find(value);
    if(!storage || !storage->storage_available){writes.unknown=true;reason(writes.reasons,InvariantReason::AliasUnavailable);return;}
    for(const auto id:storage->classes) {
        if(payload)writes.payload[id].push_back(at);
        if(descriptor)writes.descriptor[id].push_back(at);
        if(descriptor && other)writes.other_descriptor.insert(id);
    }
}
std::optional<std::string> addressed_local(const Function& fn,const DefUse& uses,ValueId address) {
    const auto* instruction=definition(fn,uses,address);if(!instruction)return {};
    if(const auto* local=std::get_if<AddressLocal>(instruction))return local->name;
    return {};
}
Writes collect_writes(const Function& fn,const Loop& loop,const LoopInvariantInputs& inputs) {
    Writes result;
    for(const auto block:loop.blocks)for(std::uint32_t index=0;index<fn.blocks[block].instructions.size();++index) {
        const auto& instruction=fn.blocks[block].instructions[index];const InstrRef at{block,index};
        std::visit([&](const auto& node){
            using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,StoreLocal>)result.locals[node.name].push_back(at);
            else if constexpr(std::is_same_v<T,LoadLocal>) {if(node.move_out)result.locals[node.name].push_back(at);}
            else if constexpr(std::is_same_v<T,FiniteFlagUpdate>)result.locals[node.flag].push_back(at);
            else if constexpr(std::is_same_v<T,StoreAddress>) {
                if(const auto local=addressed_local(fn,inputs.uses,node.address))result.locals[*local].push_back(at);
                else {result.unknown=true;reason(result.reasons,InvariantReason::ReferenceUnavailable);}
                storage_write(result,inputs.aliases,node.address,at,true,true);
            } else if constexpr(std::is_same_v<T,StoreReference> || std::is_same_v<T,BindReference> || std::is_same_v<T,BindFieldReference>) {
                result.unknown=true;reason(result.reasons,InvariantReason::ReferenceUnavailable);
            } else if constexpr(std::is_same_v<T,TensorSet>)storage_write(result,inputs.aliases,node.tensor,at,true,false);
            else if constexpr(std::is_same_v<T,FieldSet>) {result.mutable_aggregates=true;storage_write(result,inputs.aliases,node.object,at,true,true);}
            else if constexpr(std::is_same_v<T,ArraySet> || std::is_same_v<T,ArraySliceSet> || std::is_same_v<T,ArrayGrowMove>) {result.mutable_aggregates=true;storage_write(result,inputs.aliases,node.array,at,true,true);}
            else if constexpr(std::is_same_v<T,BinSet>)result.mutable_aggregates=true;
            else if constexpr(std::is_same_v<T,TensorClearGrad>)storage_write(result,inputs.aliases,node.tensor,at,false,true);
            else if constexpr(std::is_same_v<T,TensorBackward> || std::is_same_v<T,AutogradTargetClearGrad>) {
                result.unknown=true;reason(result.reasons,InvariantReason::UnknownMutation);
            } else if constexpr(std::is_same_v<T,IndirectCall>) {result.unknown=true;result.policy=true;reason(result.reasons,InvariantReason::UnknownCall);}
            else if constexpr(std::is_same_v<T,Call>) {
                const auto* summary=inputs.summaries.find(node.callee);
                if(!summary || !summary->semantic || !summary->effects_complete || !summary->descriptor_complete || (summary->effects & effect_bit::Opaque)) {
                    result.unknown=true;result.policy=true;reason(result.reasons,InvariantReason::UnknownCall);return;
                }
                const auto& semantic=*summary->semantic;
                result.policy |= (summary->effects & effect_bit::WritesPolicy)!=0;
                if(inputs.sources)if(const auto contract=inputs.sources->policies.writes_policy.find(node.callee);contract!=inputs.sources->policies.writes_policy.end())result.policy |= contract->second;
                if(semantic.mutates_ambient || !semantic.reference_shared_writes.empty() || !semantic.reference_shared_write_origins.empty()) {
                    result.unknown=true;reason(result.reasons,InvariantReason::UnknownMutation);
                }
                if(summary->semantic_parameter_for_ir_parameter.size()!=node.args.size()) {
                    result.unknown=true;reason(result.reasons,InvariantReason::UnknownMutation);return;
                }
                for(std::size_t parameter=0;parameter<node.args.size();++parameter) {
                    const auto semantic_parameter=summary->semantic_parameter_for_ir_parameter[parameter];if(!semantic_parameter){if(summary->descriptor_effects){result.unknown=true;reason(result.reasons,InvariantReason::UnknownMutation);}continue;}
                    const auto p=*semantic_parameter;
                    const bool changes=summary->descriptor_effects || (p<semantic.exposed_writes.size() && !semantic.exposed_writes[p].empty()) ||
                        (p<semantic.mutates_shared_of_parameter.size() && semantic.mutates_shared_of_parameter[p]) ||
                        (p==0 && semantic.mutates_receiver);
                    if(!changes)continue;
                    result.mutable_aggregates=true;
                    const auto& argument=node.args[parameter];const auto value=argument.writable_address.value_or(argument.value);
                    if(argument.writable_address) {
                        if(const auto local=addressed_local(fn,inputs.uses,value))result.locals[*local].push_back(at);
                        else {result.unknown=true;reason(result.reasons,InvariantReason::ReferenceUnavailable);}
                    }
                    storage_write(result,inputs.aliases,value,at,true,summary->descriptor_effects);
                }
                if(summary->descriptor_effects && node.args.empty()){result.unknown=true;reason(result.reasons,InvariantReason::UnknownMutation);}
            } else if constexpr(InstructionTraits<T>::domain==Domain::concurrency || InstructionTraits<T>::domain==Domain::random) {
                result.unknown=true;result.policy=true;reason(result.reasons,InvariantReason::UnknownMutation);
            }
        },instruction);
        // Allocation definitions create new dynamic storage instances even
        // when their numerical result happens to be invariant.
        std::visit([&](const auto& node){using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,TensorCreate> || std::is_same_v<T,TensorCast> || std::is_same_v<T,TensorGather> ||
                std::is_same_v<T,TensorScatter> || std::is_same_v<T,TensorBinary> || std::is_same_v<T,TensorCompare> || std::is_same_v<T,TensorTransfer> || std::is_same_v<T,Unary>) {
                if(const auto* storage=inputs.aliases.find(node.out))for(const auto id:storage->classes)result.created[id].push_back(at);
            }
        },instruction);
    }
    return result;
}
bool pure(const Instruction& instruction,const LoopInvariantInputs& inputs,bool policy_unchanged,bool mutable_aggregates) {
    return std::visit([&](const auto& node){
        using T=std::decay_t<decltype(node)>;constexpr auto domain=InstructionTraits<T>::domain;
        if constexpr(domain==Domain::constant)return true;
        else if constexpr(domain==Domain::numeric) {
            if constexpr(std::is_same_v<T,Unary>)return node.type.kind!=TypeKind::Tensor || policy_unchanged;
            if constexpr(std::is_same_v<T,Binary>)return node.result_type.kind!=TypeKind::Tensor || policy_unchanged;
            return true;
        } else if constexpr(std::is_same_v<T,Clone> || std::is_same_v<T,FunctionRef> || std::is_same_v<T,NumericConvert>)return true;
        else if constexpr(std::is_same_v<T,TensorCreate>)return node.fill_mode!=tensor_fill_mode::uninitialized && policy_unchanged;
        else if constexpr(std::is_same_v<T,TensorBinary> || std::is_same_v<T,TensorCompare> || std::is_same_v<T,TensorCast>)return policy_unchanged;
        else if constexpr(std::is_same_v<T,TensorReshape> || std::is_same_v<T,TensorTranspose> || std::is_same_v<T,TensorContiguous> || std::is_same_v<T,TensorIndex> || std::is_same_v<T,TensorTransfer> || std::is_same_v<T,TensorGather> || std::is_same_v<T,TensorScatter>)return true;
        else if constexpr(std::is_same_v<T,ArrayMake> || std::is_same_v<T,ArrayGet> || std::is_same_v<T,ArrayLength> || std::is_same_v<T,TupleMake> || std::is_same_v<T,TupleGet>)return !mutable_aggregates;
        else if constexpr(std::is_same_v<T,Call>) {
            const auto* summary=inputs.summaries.find(node.callee);
            return summary && summary->semantic && summary->pure_tensor && summary->semantic->shared_reads.empty() &&
                summary->semantic->reference_shared_reads.empty() && summary->semantic->reference_shared_read_origins.empty() &&
                std::none_of(summary->semantic->const_parameter_address_observed.begin(),summary->semantic->const_parameter_address_observed.end(),[](bool value){return value;}) &&
                (!(summary->effects & effect_bit::ReadsPolicy) || policy_unchanged) &&
                std::none_of(node.args.begin(),node.args.end(),[](const auto& arg){return arg.writable_address.has_value();});
        } else return false;
    },instruction);
}
bool closed(const StorageClass& storage) {
    return storage.alias_available && storage.state_identity_available && !storage.may_have_other_owners && !storage.address_exposed && !storage.escapes_function;
}
bool value_storage_unchanged(ValueId value,const AliasInfo& aliases,const Writes& writes,bool descriptor) {
    const auto* projection=aliases.find(value);if(!projection || !projection->storage_available || projection->classes.empty())return false;
    for(const auto id:projection->classes) {
        const auto storage=std::find_if(aliases.classes.begin(),aliases.classes.end(),[&](const auto& item){return item.id==id;});
        if(storage==aliases.classes.end() || !closed(*storage) || (descriptor?writes.descriptor:writes.payload).contains(id))return false;
    }
    return !writes.unknown;
}
bool same_shape(const facts::TensorFacts& a,const facts::TensorFacts& b) {
    if(!a.has_type || !b.has_type || a.type.length!=b.type.length || a.minimum_rank!=b.minimum_rank)return false;
    if(a.type.length<0 && !same_extent(a.rank_extent,b.rank_extent))return false;
    const auto equal=[](const auto& left,const auto& right){return left.size()==right.size() && std::equal(left.begin(),left.end(),right.begin(),[](const auto& x,const auto& y){return same_extent(x,y);});};
    return equal(a.extents,b.extents) && equal(a.head_extents,b.head_extents) && equal(a.tail_extents,b.tail_extents);
}
StorageProjection local_storage(std::string_view name,InstrRef at,const LoopInvariantInputs& inputs) {
    const auto source=inputs.ssa.reaching(at,name);
    if(source.available)return inputs.aliases.storage_of(source);
    const auto found=inputs.aliases.slot_storage.find(std::string(name));
    return found==inputs.aliases.slot_storage.end()?StorageProjection{}:found->second;
}
std::optional<facts::TensorFacts> local_baseline(const Function& fn,const Loop& loop,std::string_view name,const LoopInvariantInputs& inputs) {
    if(!inputs.tensors)return {};
    for(const auto& phi:inputs.ssa.phis)if(phi.block==loop.header && phi.local==name) {
        const auto found=inputs.tensors->phis.find(phi.id.value);if(found!=inputs.tensors->phis.end())return found->second;
    }
    const auto source=inputs.ssa.reaching({loop.header,0},name);
    if(source.available && source.kind==ReachingDef::Store)return inputs.tensors->at({loop.header,0},source.value);
    for(const auto& record:inputs.uses.values)if(record.definitions.size()==1) {
        const auto at=record.definitions.front();if(at.block>=fn.blocks.size() || at.index>=fn.blocks[at.block].instructions.size())continue;
        if(const auto* load=std::get_if<LoadLocal>(&fn.blocks[at.block].instructions[at.index]);load && load->name==name) {
            const auto fact=inputs.tensors->at({loop.header,0},load->out);if(fact.has_type)return fact;
        }
    }
    return {};
}
std::optional<std::uint32_t> region_for(const std::vector<InstrRef>& writes,const std::map<std::uint32_t,std::set<Site>>& regions) {
    if(writes.empty())return {};
    for(const auto& [id,members]:regions)if(std::all_of(writes.begin(),writes.end(),[&](const auto at){return members.contains({at.block,at.index});}))return id;
    return {};
}
} // namespace
LoopInvariants loop_invariants(const Function& fn,const Cfg& cfg,const DomTree& dom,const LoopNest& nest,const LoopInvariantInputs& inputs) {
    if(cfg.reachable.size()!=fn.blocks.size() || nest.reachable!=cfg.reachable || dom.scope!=DomScope::EntryPaths)
        throw std::invalid_argument("loop invariance input scope dimensions disagree");
    LoopInvariants result;
    for(std::uint32_t loop_index=0;loop_index<nest.loops.size();++loop_index) {
        const auto& loop=nest.loops[loop_index];InvariantLoop output;output.loop=loop_index;
        if(!loop.available){output.unavailable.push_back(InvariantReason::LoopUnavailable);result.loops.push_back(std::move(output));continue;}
        std::set<BlockId> blocks(loop.blocks.begin(),loop.blocks.end());const auto writes=collect_writes(fn,loop,inputs);
        output.state_complete=!writes.unknown && !writes.mutable_aggregates && inputs.aliases.complete;output.unavailable=writes.reasons;
        if(writes.mutable_aggregates)reason(output.unavailable,InvariantReason::StateIdentityUnavailable);
        output.policy_unchanged=!writes.unknown && !writes.policy;
        if(inputs.tensors && output.policy_unchanged)if(const auto* policies=inputs.tensors->policy_at({loop.header,0})) {
            output.policies=*policies;
            for(const auto latch:loop.latches) {
                const auto* end=inputs.tensors->policy_at({latch,static_cast<std::uint32_t>(fn.blocks[latch].instructions.size())});
                for(auto it=output.policies.begin();it!=output.policies.end();) {const auto found=end?end->find(it->first):facts::PolicyFacts::const_iterator{};
                    if(!end || found==end->end() || found->second!=it->second)it=output.policies.erase(it);else ++it;}
            }
            output.policy_available=!output.policies.empty();
        }
        if(!output.policy_available)reason(output.unavailable,InvariantReason::PolicyUnavailable);
        for(const auto& name:inputs.slots.ordered) {
            auto& local=output.locals[name];const auto source=inputs.ssa.reaching({loop.header,0},name);
            local.entry_available=source.available && source.kind!=ReachingDef::Undefined && source.kind!=ReachingDef::Empty;
            // Header phi entry is validated separately; a phi is not itself
            // proof that every incoming value has the same contents.
            if(source.kind==ReachingDef::Phi)for(const auto& phi:inputs.ssa.phis)if(phi.id==source.phi) {
                local.entry_available=!phi.entry || (phi.entry->available && phi.entry->kind!=ReachingDef::Undefined && phi.entry->kind!=ReachingDef::Empty);
                for(const auto& [pred,def]:phi.incoming)if(cfg.reachable[pred])local.entry_available &= def.available && def.kind!=ReachingDef::Undefined && def.kind!=ReachingDef::Empty;
            }
            const auto& slot=inputs.slots.facts.at(name);
            if(slot.reference_bound || slot.reference_binding || slot.borrowed_local || slot.borrowed_parameter || slot.writable_parameter) {
                local.entry_available=false;output.state_complete=false;reason(local.unavailable,InvariantReason::ReferenceUnavailable);
            }
            local.value_unchanged=local.entry_available && !writes.unknown && !writes.locals.contains(name);
            if(!local.entry_available)reason(local.unavailable,InvariantReason::EntryUnavailable);
            if(writes.locals.contains(name))reason(local.unavailable,InvariantReason::WrittenInLoop);
            if(writes.unknown)reason(local.unavailable,InvariantReason::UnknownMutation);
            const auto& type=inputs.slots.type.at(name);
            if(writes.mutable_aggregates && !scalar_type(type) && type.kind!=TypeKind::Tensor) {
                local.value_unchanged=false;reason(local.unavailable,InvariantReason::StateIdentityUnavailable);
            }
            const auto projection=local_storage(name,{loop.header,0},inputs);
            if(type.kind==TypeKind::Tensor) {
                if(!projection.storage_available)local.value_unchanged=false;
                for(const auto id:projection.classes)if(writes.payload.contains(id))local.value_unchanged=false;
            }
            if(type.kind!=TypeKind::Tensor || !local.entry_available || writes.unknown)continue;
            const auto baseline=local_baseline(fn,loop,name,inputs);
            if(!writes.locals.contains(name)) {
                bool descriptor=false, initialized=false;
                if(projection.storage_available && !projection.classes.empty()) {
                    descriptor=true;initialized=true;
                    for(const auto id:projection.classes) {
                        const auto cls=std::find_if(inputs.aliases.classes.begin(),inputs.aliases.classes.end(),[&](const auto& item){return item.id==id;});
                        descriptor &= cls!=inputs.aliases.classes.end() && closed(*cls) && !writes.other_descriptor.contains(id);
                        initialized &= !writes.payload.contains(id);
                    }
                }
                local.metadata={descriptor,descriptor,descriptor,descriptor,descriptor,descriptor && initialized};
                if(!descriptor)reason(local.unavailable,InvariantReason::DescriptorUnavailable);
            } else if(baseline && baseline->has_type && inputs.tensors) {
                local.metadata={true,true,true,true,true,true};
                for(const auto at:writes.locals.at(name)) {
                    const auto* store=std::get_if<StoreLocal>(&fn.blocks[at.block].instructions[at.index]);
                    if(!store){local.metadata={};break;}
                    const auto value=inputs.tensors->at(at,store->value);
                    local.metadata.shape &= same_shape(*baseline,value);
                    local.metadata.dtype &= value.has_type && baseline->type.first && value.type.first && *baseline->type.first==*value.type.first;
                    local.metadata.device &= baseline->device.kind!=facts::BackendKind::Unknown && baseline->device==value.device;
                    local.metadata.contiguous &= baseline->contiguous!=facts::Tri::Unknown && baseline->contiguous==value.contiguous;
                    local.metadata.tracked &= baseline->tracked!=facts::Tri::Unknown && baseline->tracked==value.tracked;
                    local.metadata.fully_initialized &= baseline->fully_initialized!=facts::Tri::Unknown && baseline->fully_initialized==value.fully_initialized;
                }
            } else reason(local.unavailable,InvariantReason::DescriptorUnavailable);
        }
        std::set<ValueId> invariant;
        for(const auto& record:inputs.uses.values)if(record.definitions.size()==1) {
            const auto at=record.definitions.front();if(blocks.contains(at.block) || !dom.dominates(at,{loop.header,0}))continue;
            const auto* instruction=definition(fn,inputs.uses,record.value);if(!instruction)continue;
            bool scalar=false;std::visit([&](const auto& node){
                if constexpr(requires{node.type.kind;})scalar=scalar_type(node.type);
                if constexpr(std::is_same_v<std::decay_t<decltype(node)>,ConstantBool> || std::is_same_v<std::decay_t<decltype(node)>,ConstantString>)scalar=true;
            },*instruction);
            if(scalar || (value_storage_unchanged(record.value,inputs.aliases,writes,false) && value_storage_unchanged(record.value,inputs.aliases,writes,true)))invariant.insert(record.value);
        }
        std::set<std::uint32_t> invariant_phis;
        bool changed=true;
        while(changed) {
            changed=false;
            for(const auto& phi:inputs.ssa.phis)if(blocks.contains(phi.block) && !invariant_phis.contains(phi.id.value)) {
                std::vector<ReachingDef> defs;if(phi.entry)defs.push_back(*phi.entry);
                for(const auto& [pred,def]:phi.incoming)if(cfg.reachable[pred])defs.push_back(def);
                if(defs.empty())continue;
                const auto& first=defs.front();
                const bool same=first.available && first.kind!=ReachingDef::Undefined && first.kind!=ReachingDef::Empty &&
                    std::all_of(defs.begin(),defs.end(),[&](const auto& def){return def==first;});
                if(same && ((first.kind==ReachingDef::Store && invariant.contains(first.value)) || first.kind==ReachingDef::Param ||
                    (first.kind==ReachingDef::Phi && invariant_phis.contains(first.phi.value)))){invariant_phis.insert(phi.id.value);changed=true;}
            }
            for(const auto block:loop.blocks)for(const auto& instruction:fn.blocks[block].instructions) {
                std::vector<ValueId> outputs;struct Outputs:OperandVisitor{std::vector<ValueId>& values;explicit Outputs(std::vector<ValueId>& v):values(v){}void definition(ValueId value)override{if(value)values.push_back(value);}}defs(outputs);
                visit_operands(instruction,defs);
                if(outputs.empty() || std::all_of(outputs.begin(),outputs.end(),[&](auto value){return invariant.contains(value);}))continue;
                bool accepted=false;
                if(const auto* load=std::get_if<LoadLocal>(&instruction)) {
                    const auto source=inputs.ssa.load_source.find(load->out);
                    if(source!=inputs.ssa.load_source.end() && source->second.available && !writes.unknown) {
                        const auto& def=source->second;
                        accepted=(def.kind==ReachingDef::Store && invariant.contains(def.value)) ||
                            (def.kind==ReachingDef::Phi && invariant_phis.contains(def.phi.value)) ||
                            (def.kind==ReachingDef::Param && output.locals.at(load->name).value_unchanged);
                    }
                } else if(const auto* shape=std::get_if<TensorShape>(&instruction))accepted=invariant.contains(shape->tensor) && value_storage_unchanged(shape->tensor,inputs.aliases,writes,true);
                else if(const auto* device=std::get_if<TensorDevice>(&instruction))accepted=invariant.contains(device->tensor) && value_storage_unchanged(device->tensor,inputs.aliases,writes,true);
                else if(const auto* dense=std::get_if<TensorIsContiguous>(&instruction))accepted=invariant.contains(dense->tensor) && value_storage_unchanged(dense->tensor,inputs.aliases,writes,true);
                else if(pure(instruction,inputs,output.policy_unchanged,writes.mutable_aggregates)) {
                    Reads reads;visit_operands(instruction,reads);accepted=reads.available && std::all_of(reads.values.begin(),reads.values.end(),[&](auto value){return invariant.contains(value);});
                }
                if(accepted)for(const auto value:outputs) {
                    const auto* storage=inputs.aliases.find(value);
                    if(storage && (!storage->storage_available || (!storage->classes.empty() &&
                       (!value_storage_unchanged(value,inputs.aliases,writes,false) || !value_storage_unchanged(value,inputs.aliases,writes,true)))))continue;
                    changed |= invariant.insert(value).second;
                }
            }
        }
        output.values.assign(invariant.begin(),invariant.end());
        std::map<std::uint32_t,std::set<Site>> regions;
        for(const auto& region:inputs.regions) {
            bool valid=blocks.contains(region.entry.block) && region.entry.index<fn.blocks[region.entry.block].instructions.size();
            std::set<Site> members;
            for(const auto at:region.instructions)valid &= blocks.contains(at.block) && at.index<fn.blocks[at.block].instructions.size() && dom.dominates(region.entry,at) && members.insert({at.block,at.index}).second;
            if(valid && members.contains({region.entry.block,region.entry.index}) && !regions.emplace(region.id,std::move(members)).second)throw std::invalid_argument("loop candidate region identity is duplicated");
        }
        for(auto& [name,local]:output.locals)if(!writes.unknown && local.entry_available) {
            bool covered=true;
            if(inputs.slots.type.at(name).kind==TypeKind::Tensor) {
                const auto projection=local_storage(name,{loop.header,0},inputs);covered=projection.storage_available && !projection.classes.empty();
                for(const auto id:projection.classes) {
                    const auto cls=std::find_if(inputs.aliases.classes.begin(),inputs.aliases.classes.end(),[&](const auto& item){return item.id==id;});
                    covered &= cls!=inputs.aliases.classes.end() && closed(*cls);
                }
            }
            if(covered)if(const auto found=writes.locals.find(name);found!=writes.locals.end())local.region_only_writes=region_for(found->second,regions);
        }
        for(const auto& storage:inputs.aliases.classes) {
            StorageInvariant fact;fact.storage=storage.id;fact.state_identity_available=storage.state_identity_available;
            const bool available=closed(storage) && !writes.unknown;
            fact.payload_unchanged=available && !writes.created.contains(storage.id) && !writes.payload.contains(storage.id);
            fact.descriptor_unchanged=available && !writes.created.contains(storage.id) && !writes.descriptor.contains(storage.id);
            if(writes.created.contains(storage.id))reason(fact.unavailable,InvariantReason::WrittenInLoop);
            if(!storage.alias_available)reason(fact.unavailable,InvariantReason::AliasUnavailable);
            if(!storage.state_identity_available)reason(fact.unavailable,InvariantReason::StateIdentityUnavailable);
            if(storage.may_have_other_owners || storage.address_exposed || storage.escapes_function)reason(fact.unavailable,InvariantReason::ExposedOrEscaped);
            if(writes.unknown)reason(fact.unavailable,InvariantReason::UnknownMutation);
            std::vector<InstrRef> sites;
            if(const auto found=writes.payload.find(storage.id);found!=writes.payload.end())sites=found->second;
            if(const auto found=writes.descriptor.find(storage.id);found!=writes.descriptor.end())sites.insert(sites.end(),found->second.begin(),found->second.end());
            if(const auto found=writes.created.find(storage.id);found!=writes.created.end())sites.insert(sites.end(),found->second.begin(),found->second.end());
            if(available)fact.region_only_writes=region_for(sites,regions);
            if(fact.region_only_writes) {
                fact.backedge_live_available=inputs.live.roles_complete && inputs.live.storage_complete;
                const auto& members=regions.at(*fact.region_only_writes);
                for(const auto latch:loop.latches) {
                    if(fn.blocks[latch].instructions.empty()){fact.backedge_live_available=false;continue;}
                    const auto* live=inputs.live.after({latch,static_cast<std::uint32_t>(fn.blocks[latch].instructions.size()-1)});
                    if(!live || !live->storage_available){fact.backedge_live_available=false;continue;}
                    bool live_sources_known=false;
                    for(const auto value:live->values) {
                        const auto* alias=inputs.aliases.find(value);if(!alias || !alias->storage_available){fact.backedge_live_available=false;continue;}
                        if(std::find(alias->classes.begin(),alias->classes.end(),storage.id)==alias->classes.end())continue;
                        live_sources_known=true;
                        const auto* record=inputs.uses.find(value);
                        if(!record || record->definitions.size()!=1){fact.backedge_live_available=false;continue;}
                        const auto at=record->definitions.front();if(!members.contains({at.block,at.index}))fact.outside_region_live=true;
                    }
                    const InstrRef end{latch,static_cast<std::uint32_t>(fn.blocks[latch].instructions.size())};
                    for(const auto& local:live->locals) {
                        const auto source=inputs.ssa.reaching(end,local);const auto projection=local_storage(local,end,inputs);
                        if(!projection.storage_available){fact.backedge_live_available=false;continue;}
                        if(std::find(projection.classes.begin(),projection.classes.end(),storage.id)==projection.classes.end())continue;
                        live_sources_known=true;
                        if(!source.available){fact.backedge_live_available=false;continue;}
                        if(source.kind==ReachingDef::Store) {
                            const auto* record=inputs.uses.find(source.value);
                            if(!record || record->definitions.size()!=1){fact.backedge_live_available=false;continue;}
                            const auto at=record->definitions.front();if(!members.contains({at.block,at.index}))fact.outside_region_live=true;
                        } else if(source.kind==ReachingDef::Param || source.kind==ReachingDef::Phi)fact.outside_region_live=true;
                    }
                    if(std::find(live->storage.begin(),live->storage.end(),storage.id)!=live->storage.end() && !live_sources_known)fact.backedge_live_available=false;
                }
                if(fact.outside_region_live)reason(fact.unavailable,InvariantReason::BackedgeLive);
            } else if(!sites.empty())reason(fact.unavailable,InvariantReason::OutsideRegionWrite);
            output.storage.push_back(std::move(fact));
        }
        result.loops.push_back(std::move(output));
    }
    return result;
}
} // namespace quidra::ir::analysis
