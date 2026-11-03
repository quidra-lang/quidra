#include "lowering/binding_origins.hpp"
#include "quidra/ir/visit.hpp"

#include <algorithm>
#include <limits>

namespace quidra::lowering {
using namespace ir;
using namespace ir::analysis;
void BindingOriginRecorder::header(const Function& fn,const SemanticBinding& binding) {
    if(!fn.declaration)return;
    FunctionBindingSource source;source.declaration=*fn.declaration;source.complete=true;
    source.semantic_parameter_for_ir_parameter=binding.semantic_parameter_for_ir_parameter;
    source.availability.parameter_map=true;source.availability.local_storage_paths=true;
    source.availability.collections=true;source.availability.rebind_events=true;source.availability.traversals=true;
    for(std::size_t i=0;i<fn.parameters.size();++i)if(i<binding.semantic_parameter_for_ir_parameter.size())
        if(const auto index=binding.semantic_parameter_for_ir_parameter[i])
            source.local_storage_paths.emplace(fn.parameters[i].name,semantics::StoragePath{semantics::StorageRoot::parameter(*index),{}});
    functions_.emplace(fn.name,std::move(source));
}
void BindingOriginRecorder::local(const Function& fn,const std::string& storage,const std::string& source) {
    if(const auto found=functions_.find(fn.name);found!=functions_.end()) {
        auto& row=found->second;
        const semantics::StoragePath path{semantics::StorageRoot::local(source),{}};
        // Source spellings reused across scopes lack a captured declaration
        // identity. Do not collapse distinct bindings into one proven holder.
        for(const auto& [existing_storage,existing_path]:row.local_storage_paths)
            if(existing_storage!=storage && existing_path==path)row.availability.local_storage_paths=false;
        row.local_storage_paths.insert_or_assign(storage,path);
    }
}
void BindingOriginRecorder::local_unknown(const Function& fn,const std::string& storage) {
    if(const auto found=functions_.find(fn.name);found!=functions_.end()) {
        found->second.local_storage_paths.erase(storage);
        found->second.availability.local_storage_paths=false;
    }
}
void BindingOriginRecorder::collection(const Function& fn,const Type& root_type,CollectionOrigin origin) {
    if(const auto found=functions_.find(fn.name);found!=functions_.end()) {
        BindingTraversalOrigin query;query.kind=BindingTraversalOrigin::Kind::Collect;query.at=origin.at;
        query.root=origin.root;query.output=origin.array;query.root_type=root_type;query.target=origin.target;
        query.paths=origin.paths;query.complete=origin.complete;found->second.traversals.push_back(std::move(query));
        found->second.collections.push_back(std::move(origin));
    }
}
void BindingOriginRecorder::rebind(const Function& fn,BindingRebindEvent event) {
    if(const auto found=functions_.find(fn.name);found!=functions_.end())found->second.rebind_events.push_back(std::move(event));
}
InstrRef BindingOriginRecorder::site(const FunctionBuilder& builder) {
    if(!builder.function() || !builder.block())throw std::logic_error("binding origin lacks an active IR block");
    const auto& blocks=builder.function()->blocks;
    for(std::size_t b=0;b<blocks.size();++b)if(&blocks[b]==builder.block()) {
        if(b>std::numeric_limits<BlockId>::max() || blocks[b].instructions.size()>std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("binding origin IR dimensions are out of range");
        return {static_cast<BlockId>(b),static_cast<std::uint32_t>(blocks[b].instructions.size())};
    }
    throw std::logic_error("binding origin block is not owned by its function");
}
void BindingOriginRecorder::traversal(const FunctionBuilder& builder,const TraversalPlan& plan,
    BindingTraversalOrigin::Kind kind,ValueId root,std::optional<ValueId> output,std::optional<ValueId> address) {
    const auto found=functions_.find(builder.function()->name);if(found==functions_.end())return;
    BindingTraversalOrigin origin;origin.kind=kind;origin.at=site(builder);origin.root=root;
    origin.output=output;origin.root_address=address;origin.root_type=plan.root;origin.target=plan.element;
    origin.complete=true;origin.writable=plan.writable;
    const auto walk=[&](const auto& self,const TraversalNode& node,std::vector<semantics::PathStep> path)->void {
        switch(node.kind) {
            case TraversalNode::Kind::Visit:origin.paths.push_back(std::move(path));return;
            case TraversalNode::Kind::Field:path.push_back(semantics::PathStep::field_named(node.name));break;
            case TraversalNode::Kind::TupleElement:path.push_back(semantics::PathStep::tuple_element(node.index));break;
            case TraversalNode::Kind::Array:origin.complete=false;path.push_back(semantics::PathStep::any_element());break;
            case TraversalNode::Kind::Fields:break;
        }
        for(const auto& child:node.children)self(self,child,path);
    };
    walk(walk,plan.tree,{});found->second.traversals.push_back(std::move(origin));
}
void BindingOriginRecorder::split_shape_entry(const std::string& wrapper,const std::string& body,
    std::size_t first_block,std::size_t first_instruction) {
    const auto source=functions_.find(wrapper);if(source==functions_.end())return;
    auto copied=source->second;copied.collections.clear();copied.rebind_events.clear();copied.traversals.clear();
    const auto partition=[&](auto& input,auto& destination) {
        std::erase_if(input,[&](const auto& original) {
            if(original.at.block<first_block || (original.at.block==first_block && original.at.index<first_instruction))return false;
            auto relocated=original;relocated.at.block-=static_cast<BlockId>(first_block);
            if(!relocated.at.block)relocated.at.index-=static_cast<std::uint32_t>(first_instruction);
            destination.push_back(std::move(relocated));return true;
        });
    };
    partition(source->second.collections,copied.collections);partition(source->second.rebind_events,copied.rebind_events);
    partition(source->second.traversals,copied.traversals);functions_.emplace(body,std::move(copied));
}
void BindingOriginRecorder::remap_reference_storage(const std::string& function,
    const std::vector<std::vector<std::size_t>>& boundaries,const std::set<std::string>& removed) {
    const auto found=functions_.find(function);if(found==functions_.end())return;
    auto& source=found->second;
    const auto remap=[&](InstrRef& at) {
        if(at.block>=boundaries.size() || at.index>=boundaries[at.block].size() ||
           boundaries[at.block][at.index]>std::numeric_limits<std::uint32_t>::max())
            throw std::logic_error("binding origin reference-storage boundary is unavailable");
        at.index=static_cast<std::uint32_t>(boundaries[at.block][at.index]);
    };
    for(auto& origin:source.collections)remap(origin.at);
    for(auto& origin:source.traversals)remap(origin.at);
    // Rebind events are instruction sites, so finish() matches the exact
    // typed bind operands within the rewritten body rather than boundary+1.
    for(const auto& local:removed)if(source.local_storage_paths.erase(local))source.availability.local_storage_paths=false;
}
std::shared_ptr<const BindingOriginSnapshots> BindingOriginRecorder::finish(const Module& module,
    const SummaryBindings& bindings,const semantics::BindingEntryInitializations* entries) const {
    auto result=std::make_shared<BindingOriginSnapshots>();
    result->class_fields=canonical_encode_value(module.classes).bytes;
    for(const auto& fn:module.functions) {
        const auto pending=functions_.find(fn.name);if(pending==functions_.end())continue;
        BindingOriginSnapshot snapshot;snapshot.source=pending->second;
        auto& source=snapshot.source;
        const auto issue=[&](BindingSourceComponent component,BindingGap gap) {snapshot.unavailable.push_back({component,gap,fn.name});};
        const auto binding=bindings.find(fn.name);
        if(binding==bindings.end() || binding->second.semantic_parameter_for_ir_parameter.size()!=fn.parameters.size()) {
            source.complete=false;source.availability.parameter_map=false;issue(BindingSourceComponent::ParameterMap,BindingGap::MissingOrigin);
        } else {
            source.semantic_parameter_for_ir_parameter=binding->second.semantic_parameter_for_ir_parameter;
            source.availability.parameter_map=true;
            if(source.declaration.kind==DeclarationIdentity::Kind::Method) {
                std::size_t count=0;
                for(std::size_t i=0;i<source.semantic_parameter_for_ir_parameter.size();++i)
                    if(source.semantic_parameter_for_ir_parameter[i]==0) {++count;source.receiver_ir_parameter=static_cast<std::uint32_t>(i);}
                source.availability.receiver=count==1;
                if(!source.availability.receiver){source.receiver_ir_parameter.reset();issue(BindingSourceComponent::Receiver,BindingGap::Ambiguous);}
            }
            if(entries)if(const auto entry=entries->functions.find(binding->second.semantic_key);entry!=entries->functions.end()) {
                source.availability.validated_initialization=entry->second.complete;
                if(entry->second.complete)for(std::size_t i=0;i<source.semantic_parameter_for_ir_parameter.size();++i)
                    if(const auto semantic=source.semantic_parameter_for_ir_parameter[i])
                        if(const auto paths=entry->second.parameters.find(*semantic);paths!=entry->second.parameters.end())
                            source.validated_initialization.emplace(static_cast<std::uint32_t>(i),paths->second);
            }
        }
        struct Defined final:OperandVisitor {
            std::map<ValueId,std::vector<InstrRef>> values;
            std::set<std::string,std::less<>> locals;
            InstrRef at;
            void definition(ValueId value)override{values[value].push_back(at);}
            void local(std::string_view name,LocalRole)override{locals.emplace(name);}
        } defined;
        for(const auto& parameter:fn.parameters)defined.locals.insert(parameter.name);
        for(BlockId b=0;b<fn.blocks.size();++b)for(std::uint32_t i=0;i<fn.blocks[b].instructions.size();++i) {
            defined.at={b,i};visit_operands(fn.blocks[b].instructions[i],defined);
        }
        // The explicit reference-storage boundary map preserved query sites.
        // Validate actual SSA definitions and authored order without replacing
        // those sites or decoding generated storage/source coordinates.
        for(auto& collection:source.collections) {
            if(!collection.complete)issue(BindingSourceComponent::Collections,BindingGap::DynamicIndex);
            const auto value=defined.values.find(collection.array),root=defined.values.find(collection.root);
            if(value==defined.values.end()||value->second.size()!=1||root==defined.values.end()||root->second.size()!=1||
               value->second[0].index==std::numeric_limits<std::uint32_t>::max()) {
                collection.complete=false;issue(BindingSourceComponent::Collections,BindingGap::MissingOrigin);continue;
            }
            if(collection.at.block>=fn.blocks.size() || collection.at.index>fn.blocks[collection.at.block].instructions.size() ||
               value->second[0].block!=collection.at.block || value->second[0].index>=collection.at.index) {
                collection.complete=false;issue(BindingSourceComponent::Collections,BindingGap::MissingOrigin);
            }
        }
        for(auto& traversal:source.traversals) {
            const auto root=defined.values.find(traversal.root);
            const bool known=root!=defined.values.end() && root->second.size()==1 &&
                traversal.at.block<fn.blocks.size() && traversal.at.index<=fn.blocks[traversal.at.block].instructions.size();
            if(!known){traversal.complete=false;issue(BindingSourceComponent::Traversals,BindingGap::MissingOrigin);}
            if(traversal.output) {
                const auto value=defined.values.find(*traversal.output);
                if(value==defined.values.end()||value->second.size()!=1 || value->second[0].block!=traversal.at.block || value->second[0].index>=traversal.at.index) {
                    traversal.complete=false;issue(BindingSourceComponent::Traversals,BindingGap::MissingOrigin);
                }
            }
            if(!traversal.complete)issue(BindingSourceComponent::Traversals,BindingGap::DynamicIndex);
        }
        // A boxed or eliminated local cannot retain its old storage contract.
        // Other known bindings are still owned, but whole-map availability is
        // false until an explicit projected-storage producer exists.
        std::erase_if(source.local_storage_paths,[&](const auto& path){
            if(defined.locals.contains(path.first))return false;
            source.availability.local_storage_paths=false;issue(BindingSourceComponent::LocalStoragePaths,BindingGap::MissingOrigin);return true;
        });
        for(auto& event:source.rebind_events) {
            std::vector<InstrRef> matches;
            for(BlockId b=0;b<fn.blocks.size();++b)for(std::uint32_t i=0;i<fn.blocks[b].instructions.size();++i) {
                const auto& ins=fn.blocks[b].instructions[i];
                if(event.kind==BindingRebindEvent::Kind::Reference) {
                    if(const auto* node=std::get_if<BindReference>(&ins);node && node->name==event.local && node->address==event.address)matches.push_back({b,i});
                } else if(const auto* node=std::get_if<BindFieldReference>(&ins);node && node->object==event.object && node->index==event.field && node->address==event.address)matches.push_back({b,i});
            }
            if(matches.size()==1)event.at=matches[0];else {source.availability.rebind_events=false;issue(BindingSourceComponent::RebindEvents,BindingGap::Ambiguous);}
        }
        if(fn.external_symbol) {
            source.availability.local_storage_paths=false;source.availability.collections=false;source.availability.rebind_events=false;source.availability.traversals=false;
        }
        if(!source.availability.local_storage_paths)issue(BindingSourceComponent::LocalStoragePaths,BindingGap::MissingOrigin);
        if(!source.availability.validated_initialization)issue(BindingSourceComponent::ValidatedInitialization,BindingGap::MissingOrigin);
        // No source method role exists. Actual RebindStmt events are retained
        // separately and never turn an ordinary function into a rebind method.
        source.rebind=false;source.availability.rebind_role=false;
        snapshot.body_fields=binding_origin_body_fields(fn);
        result->functions.emplace(fn.name,std::move(snapshot));
    }
    return result;
}
} // namespace quidra::lowering
