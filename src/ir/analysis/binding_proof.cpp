#include "ir/analysis/binding_proof.hpp"

#include <algorithm>
#include <stdexcept>
#include <set>

namespace quidra::ir::analysis {
namespace {
void gap(std::vector<BindingGap>& gaps,BindingGap why){if(std::find(gaps.begin(),gaps.end(),why)==gaps.end())gaps.push_back(why);}
const BindingClass* layout(const BindingSources& s,const Type& t){const auto found=s.classes.find(t.class_name);return t.kind==TypeKind::Class && found!=s.classes.end()?&found->second:nullptr;}
const FunctionBindingSource* origin(const BindingSources& s,const Function& f){const auto p=s.functions.find(f.name);return p==s.functions.end()?nullptr:&p->second;}
bool owner_matches(const FunctionBindingSource& f,const ClassIdentity& c){return f.declaration.owner_class==c.source_name && f.declaration.package==c.package && f.declaration.module_path==c.module_path && f.declaration.generic_args==c.generic_arguments;}
std::string semantic_owner(const Function& fn,BlockId block) {
    const InlineScope* best=nullptr;std::size_t best_depth=0;
    for(const auto& scope:fn.inline_scopes) {
        if(std::find(scope.blocks.begin(),scope.blocks.end(),fn.blocks.at(block).label)==scope.blocks.end())continue;
        std::set<std::uint64_t> seen;const auto* current=&scope;std::size_t depth=1;
        while(current->parent){if(!seen.insert(current->identity).second)return {};
            const auto parent=std::find_if(fn.inline_scopes.begin(),fn.inline_scopes.end(),[&](const auto& p){return p.identity==current->parent;});
            if(parent==fn.inline_scopes.end())return {};
            current=&*parent;++depth;}
        if(depth==best_depth && best && best->identity!=scope.identity)return {};
        if(depth>best_depth){best=&scope;best_depth=depth;}
    }
    return best?best->callee:fn.name;
}
bool own_fresh_target(const Function& fn,const ObjectWrite& write) {
    if(!write.object.allocation || write.object.allocation->kind!=ObjectRoot::Allocation || write.object.allocation->function!=fn.name)return false;
    const auto birth=write.object.allocation->at;
    return birth.block<fn.blocks.size() && birth.index<fn.blocks[birth.block].instructions.size() &&
        std::holds_alternative<ClassMake>(fn.blocks[birth.block].instructions[birth.index]) &&
        semantic_owner(fn,birth.block)==write.semantic_function;
}
bool own_receiver(const Function& fn,const FunctionBindingSource& source,const ObjectWrite& write) {
    return source.receiver_ir_parameter && write.object.sources.size()==1 && write.object.sources[0].steps.empty() &&
        write.object.sources[0].root.kind==ObjectRoot::Parameter && write.object.sources[0].root.function==fn.name &&
        write.object.sources[0].root.parameter==*source.receiver_ir_parameter && write.semantic_function==fn.name;
}
struct Key {ClassIdentity owner;std::size_t field{};auto operator<=>(const Key&)const=default;};
}
BindingProof binding_proof(const Module& module,const ModuleSummaries& summaries,const BindingSources& sources){
    std::vector<ObjectProvenance> observations;
    for(const auto& fn:module.functions){const auto cfg=build_cfg(fn);const auto dom=dominators(cfg);const auto slots=classify_slots(fn);const auto ssa=build_local_ssa(fn,cfg,dom,slots);const auto uses=def_use(fn,&module);
        observations.push_back(object_provenance(fn,cfg,dom,ssa,slots,uses,summaries,sources));}
    return binding_proof(module,summaries,sources,std::move(observations));
}
BindingProof binding_proof(const Module& module,const ModuleSummaries& summaries,const BindingSources& sources,std::vector<ObjectProvenance> observations){
    if(observations.size()!=module.functions.size())throw std::invalid_argument("binding observation dimensions disagree");
    BindingProof result;result.objects=std::move(observations);std::map<Key,std::size_t> fields;std::map<Key,const BindingField*> contracts;
    for(const auto& [name,c]:sources.classes){(void)name;for(std::size_t i=0;i<c.fields.size();++i){Key key{c.identity,i};if(fields.contains(key))continue;
        FieldProof proof;proof.owner=c.identity;proof.field=i;proof.name=c.fields[i].name;proof.available=c.complete;
        if(!c.complete)proof.gaps.push_back(BindingGap::MissingClass);
        fields.emplace(key,result.fields.size());contracts.emplace(key,&c.fields[i]);result.fields.push_back(std::move(proof));}}
    for(std::size_t f=0;f<module.functions.size();++f){const auto& fn=module.functions[f];const auto& facts=result.objects[f];const auto* source=origin(sources,fn);
        const auto& reachable=facts.reachable;
        for(const auto& write:facts.writes){const auto* c=layout(sources,write.object.type);if(!c || write.field>=c->fields.size())continue;
            auto& proof=result.fields[fields.at({c->identity,write.field})];
            const auto semantic=sources.functions.find(write.semantic_function);const auto* writer_source=semantic==sources.functions.end()?nullptr:&semantic->second;
            const bool allocation=write.object.allocation && write.object.allocation->kind==ObjectRoot::Allocation && write.object.allocation->at==write.at;
            const bool constructor=allocation || (writer_source && writer_source->complete && writer_source->declaration.kind==DeclarationIdentity::Kind::Constructor && owner_matches(*writer_source,c->identity) && own_fresh_target(fn,write));
            const bool rebind=writer_source && writer_source->complete && writer_source->rebind && writer_source->declaration.kind==DeclarationIdentity::Kind::Method && writer_source->declaration.mutation_marker && owner_matches(*writer_source,c->identity) && own_receiver(fn,*writer_source,write);
            proof.writers.push_back({fn.name,write.at,write.semantic_function,constructor,rebind,write.unknown || (!allocation && (!writer_source || !writer_source->complete))});
            if(write.semantic_function.empty())gap(proof.gaps,BindingGap::InlineOriginUnavailable);
        }
        auto identity_layout=[&](const ClassIdentity& identity)->const BindingClass* {
            for(const auto& [name,c]:sources.classes){(void)name;if(c.identity==identity)return &c;}return nullptr;};
        auto path_writer=[&](const ObjectPath& object,const std::vector<semantics::PathStep>& suffix,FieldWriter writer){
            if(!object.root.class_identity)return;
            const auto* c=identity_layout(*object.root.class_identity);
            auto steps=object.steps;steps.insert(steps.end(),suffix.begin(),suffix.end());
            if(steps.empty()){if(c)for(std::size_t i=0;i<c->fields.size();++i)result.fields[fields.at({c->identity,i})].writers.push_back(writer);return;}
            for(std::size_t i=0;c && i<steps.size();++i){if(steps[i].kind!=semantics::StepKind::Field){
                    for(std::size_t j=0;j<c->fields.size();++j){auto unknown=writer;unknown.unknown=true;result.fields[fields.at({c->identity,j})].writers.push_back(unknown);}return;}
                const auto named=std::find_if(c->fields.begin(),c->fields.end(),[&](const auto& f){return f.name==steps[i].field;});if(named==c->fields.end())return;
                const auto ordinal=static_cast<std::size_t>(named-c->fields.begin());
                const bool terminal=i+1==steps.size();const bool array_element=named->type.kind==TypeKind::Array && !terminal;
                if(terminal || array_element){result.fields[fields.at({c->identity,ordinal})].writers.push_back(writer);return;}
                if(named->reference){auto unknown=writer;unknown.unknown=true;result.fields[fields.at({c->identity,ordinal})].writers.push_back(unknown);return;}
                c=layout(sources,named->type);
            }
        };
        for(const auto& exposure:facts.exposures){const auto* c=layout(sources,exposure.object.type);
            if(c)for(std::size_t i=0;i<c->fields.size();++i)result.fields[fields.at({c->identity,i})].writers.push_back({fn.name,exposure.at,fn.name,false,false,true});
            for(const auto& p:exposure.object.sources)path_writer(p,{}, {fn.name,exposure.at,fn.name,false,false,true});
        }
        for(BlockId b=0;b<fn.blocks.size();++b)if(reachable[b])for(std::uint32_t i=0;i<fn.blocks[b].instructions.size();++i) {
            // Replacing an element in a published field changes the static list,
            // even when the containing array binding itself is not reassigned.
            if(const auto* set=std::get_if<ArraySet>(&fn.blocks[b].instructions[i])){
                const auto array=facts.at({b,i},set->array);for(const auto& path:array.sources)path_writer(path,{}, {fn.name,{b,i},fn.name,false,false,true});
            }
        }
        // Checked paths are copied exactly. Only a replacement of the named
        // field/array binding counts here; deeper payload writes are distinct.
        for(BlockId b=0;b<fn.blocks.size();++b)if(reachable[b])for(std::uint32_t i=0;i<fn.blocks[b].instructions.size();++i)if(const auto* call=std::get_if<Call>(&fn.blocks[b].instructions[i])){
            const auto* summary=summaries.find(call->callee);if(!summary || !summary->semantic || !summary->effects_complete || (summary->effects & effect_bit::Opaque))continue;
            for(std::size_t p=0;p<call->args.size() && p<summary->semantic_parameter_for_ir_parameter.size();++p){const auto mapped=summary->semantic_parameter_for_ir_parameter[p];if(!mapped)continue;
                const auto object=facts.at({b,i},call->args[p].writable_address.value_or(call->args[p].value));
                const auto index=*mapped;const auto& effect=*summary->semantic;
                const auto callee_source=sources.functions.find(call->callee);const auto* callee=callee_source==sources.functions.end()?nullptr:&callee_source->second;
                for(const auto& object_path:object.sources)if(object_path.root.class_identity){
                    const auto& identity=*object_path.root.class_identity;
                    const bool ctor=false; // Caller arguments are foreign to the callee constructor allocation.
                    const bool rebind=callee && callee->complete && callee->rebind && callee->declaration.kind==DeclarationIdentity::Kind::Method && callee->declaration.mutation_marker && owner_matches(*callee,identity) && callee->receiver_ir_parameter==p;
                    if(index<effect.exposed_writes.size())for(const auto& path:effect.exposed_writes[index])path_writer(object_path,path.steps,{fn.name,{b,i},call->callee,ctor,rebind,false});
                    if((index<effect.mutates_shared_of_parameter.size() && effect.mutates_shared_of_parameter[index]) ||
                       (index<effect.reference_target_escapes.size() && effect.reference_target_escapes[index]))path_writer(object_path,{}, {fn.name,{b,i},call->callee,false,false,true});
                }
            }
        }
        if(source)for(const auto& query:source->collections){FixedCollection collection;collection.function=fn.name;collection.at=query.at;collection.array=query.array;collection.target=query.target;
            const auto array=facts.at(query.at,query.array),root=facts.at(query.at,query.root);const auto* target=layout(sources,query.target);
            collection.available=source->complete && query.complete && target && target->complete && array.available && array.array_closed && array.type.kind==TypeKind::Array && array.type.first && root.available && root.sources.size()==1 && array.elements.size()==query.paths.size();
            collection.paths_available=collection.available;collection.initialized=true;
            if(!source->complete || !query.complete)gap(collection.gaps,BindingGap::MissingOrigin);
            if(!target || !target->complete)gap(collection.gaps,BindingGap::MissingClass);
            if(!array.array_closed)gap(collection.gaps,BindingGap::ArrayAlias);
            if(root.sources.size()!=1 || array.elements.size()!=query.paths.size())gap(collection.gaps,BindingGap::Ambiguous);
            for(std::size_t i=0;i<array.elements.size();++i){const auto& value=array.elements[i];collection.values.push_back(value);
                bool matches=value.available && value.type.kind==TypeKind::Class && value.sources.size()==1 && i<query.paths.size() && root.sources.size()==1;
                if(matches){auto expected=root.sources.front();expected.steps.insert(expected.steps.end(),query.paths[i].begin(),query.paths[i].end());matches &= value.sources[0]==expected;
                    const auto* actual=layout(sources,value.type);matches &= actual && target && actual->identity==target->identity;}
                collection.paths_available &= matches;if(!matches)gap(collection.gaps,BindingGap::Ambiguous);
                collection.initialized &= value.initialized;if(!value.initialized)gap(collection.gaps,BindingGap::InitializationUnavailable);
                if(!value.references_closed){collection.available=false;gap(collection.gaps,BindingGap::ReferenceUnavailable);}
                if(value.sources.size()==1)collection.elements.push_back(value.sources.front());
            }
            collection.available &= collection.paths_available && collection.initialized;
            result.collections.push_back(std::move(collection));
        }
    }
    bool changed=true;
    while(changed){changed=false;for(std::size_t i=0;i<module.functions.size();++i){const auto& fn=module.functions[i];const auto* source=origin(sources,fn);
        if(!source || !source->complete || source->declaration.kind!=DeclarationIdentity::Kind::Constructor || result.constructor_allocations[fn.name])continue;
        const auto& returns=result.objects[i].returns;const bool fresh=!returns.empty() && std::all_of(returns.begin(),returns.end(),[&](const auto& f){return f.available && f.allocation &&
            (f.allocation_identity_available || (f.allocation->kind==ObjectRoot::ConstructorCall && result.constructor_allocations[f.allocation->constructor_function]));});
        if(fresh){result.constructor_allocations[fn.name]=true;changed=true;}
    }}
    for(auto& field:result.fields){field.constructor_or_rebind_only=field.available && std::all_of(field.writers.begin(),field.writers.end(),[](const auto& w){return !w.unknown && (w.construction || w.rebind);});
        for(const auto& writer:field.writers)if(writer.unknown)gap(field.gaps,BindingGap::UnknownWriter);else if(!writer.construction && !writer.rebind)gap(field.gaps,BindingGap::OrdinaryWriter);
        field.available &= field.constructor_or_rebind_only;
    }
    for(auto& collection:result.collections)for(const auto& element:collection.elements){
        const BindingClass* c=nullptr;if(element.root.class_identity)for(const auto& [name,owner]:sources.classes){(void)name;if(owner.identity==*element.root.class_identity){c=&owner;break;}}
        for(const auto& step:element.steps){if(!c || step.kind!=semantics::StepKind::Field){collection.available=false;gap(collection.gaps,BindingGap::DynamicIndex);break;}
            const auto field=std::find_if(c->fields.begin(),c->fields.end(),[&](const auto& f){return f.name==step.field;});
            if(field==c->fields.end() || field->reference){collection.available=false;gap(collection.gaps,BindingGap::ReferenceUnavailable);break;}
            const auto& proof=result.fields[fields.at({c->identity,static_cast<std::size_t>(field-c->fields.begin())})];
            if(!proof.constructor_or_rebind_only){collection.available=false;gap(collection.gaps,BindingGap::OrdinaryWriter);}
            c=layout(sources,field->type);
        }
    }
    for(const auto& field:result.fields){
        const auto* contract=contracts.at({field.owner,field.field});if(!contract->is_private || contract->reference || contract->type.kind!=TypeKind::Array)continue;
        BoundField bound;static_cast<FieldProof&>(bound)=field;
        bool closed=true;
        for(std::size_t f=0;f<module.functions.size();++f)for(const auto& write:result.objects[f].writes){const auto* c=layout(sources,write.object.type);if(!c || c->identity!=field.owner || write.field!=field.field)continue;
            bool found=false;
            for(std::size_t i=0;i<result.collections.size();++i){const auto& collection=result.collections[i];if(collection.function!=module.functions[f].name)continue;
                const auto array=result.objects[f].at(collection.at,collection.array);
                if(write.value.allocation && array.allocation==write.value.allocation){found=true;closed &= collection.available;if(std::find(bound.collections.begin(),bound.collections.end(),i)==bound.collections.end())bound.collections.push_back(i);}
            }
            if(!found){closed=false;gap(bound.gaps,BindingGap::MissingOrigin);}
        }
        bound.available &= closed && !bound.collections.empty();if(bound.collections.empty())gap(bound.gaps,BindingGap::MissingOrigin);result.bound_fields.push_back(std::move(bound));
    }
    return result;
}
} // namespace quidra::ir::analysis
