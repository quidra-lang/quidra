#include "ir/analysis/object_provenance.hpp"
#include "ir/inline_binding_origins.hpp"
#include "semantics/interface_contracts.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace quidra::ir::analysis {
namespace {
void gap(ObjectFact& f,BindingGap why){if(std::find(f.gaps.begin(),f.gaps.end(),why)==f.gaps.end())f.gaps.push_back(why);}
ObjectFact unavailable(Type type,BindingGap why){ObjectFact f;f.type=std::move(type);gap(f,why);return f;}
const BindingClass* layout(const BindingSources& sources,const Type& type){
    if(type.kind!=TypeKind::Class)return nullptr;
    const auto found=sources.classes.find(type.class_name);return found==sources.classes.end()?nullptr:&found->second;
}
const FunctionBindingSource* function_source(const BindingSources& sources,std::string_view name){
    const auto found=sources.functions.find(name);return found==sources.functions.end()?nullptr:&found->second;
}
bool references(const BindingClass* c){return c && std::any_of(c->fields.begin(),c->fields.end(),[](const auto& f){return f.reference;});}
using Site=std::pair<BlockId,std::uint32_t>;
struct Env {
    std::map<ValueId,ObjectFact> values;
    std::map<std::string,ObjectFact,std::less<>> locals,references;
    std::set<std::string,std::less<>> raw_bindings;
    std::map<Site,std::vector<ObjectFact>> fields;
    bool operator==(const Env&)const=default;
};
struct Engine {
    const Function& fn;const Cfg& cfg;const DomTree& dom;const DefUse& uses;
    const ModuleSummaries& summaries;const BindingSources& sources;const FunctionBindingSource* origin;
    ObjectProvenance out;std::vector<std::optional<Env>> exits;
    Engine(const Function& f,const Cfg& c,const DomTree& d,const DefUse& u,const ModuleSummaries& m,const BindingSources& s)
        :fn(f),cfg(c),dom(d),uses(u),summaries(m),sources(s),origin(function_source(s,f.name)),exits(f.blocks.size()) {
        if(c.reachable.size()!=f.blocks.size() || d.scope!=DomScope::EntryPaths)throw std::invalid_argument("object provenance CFG scope disagrees");
        out.scopes.resize(f.blocks.size());out.reachable=c.reachable;out.dominance=d;
        for(const auto& record:u.values)if(record.definitions.size()==1)out.definitions.emplace(record.value,record.definitions.front());
    }
    std::string semantic_owner(BlockId block)const {
        const InlineScope* best=nullptr;std::size_t best_depth=0;
        for(const auto& scope:fn.inline_scopes) {
            if(std::find(scope.blocks.begin(),scope.blocks.end(),fn.blocks.at(block).label)==scope.blocks.end())continue;
            std::set<std::uint64_t> seen;const auto* current=&scope;std::size_t depth=1;
            while(current->parent) {
                if(!seen.insert(current->identity).second)return {};
                const auto parent=std::find_if(fn.inline_scopes.begin(),fn.inline_scopes.end(),[&](const auto& p){return p.identity==current->parent;});
                if(parent==fn.inline_scopes.end())return {};
                current=&*parent;++depth;
            }
            if(depth==best_depth && best && best->identity!=scope.identity)return {};
            if(depth>best_depth){best=&scope;best_depth=depth;}
        }
        return best?best->callee:fn.name;
    }
    const FunctionBindingSource* semantic_source(BlockId block)const {
        if(const auto* scoped=scoped_binding_source(sources,fn,block))return &scoped->source;
        return semantic_owner(block)==fn.name?origin:nullptr;
    }
    std::uint64_t semantic_scope(BlockId block)const {
        if(const auto* scoped=inline_binding_scope(fn,block))return scoped->identity;
        return 0;
    }
    ObjectRoot root(ObjectRoot::Kind kind,InstrRef at,const Type& type)const{
        ObjectRoot r;r.kind=kind;r.function=fn.name;r.at=at;
        if(const auto* c=layout(sources,type))r.class_identity=c->identity;
        return r;
    }
    ObjectFact materialize(ObjectFact f,const Env& env)const{
        if(f.allocation && f.allocation->function==fn.name)if(const auto fields=env.fields.find({f.allocation->at.block,f.allocation->at.index});fields!=env.fields.end()) {
            f.elements=fields->second;
            const auto* c=layout(sources,f.type);
            if(c && c->complete) {
                f.initialized=true;
                for(std::size_t i=0;i<c->fields.size();++i)if(!c->fields[i].reference)
                    f.initialized &= i<f.elements.size() && f.elements[i].initialized;
            } else f.initialized=false;
        }
        if(f.type.kind==TypeKind::Class){const auto* c=layout(sources,f.type);f.references_closed=c && c->complete;
            if(c)for(std::size_t i=0;i<c->fields.size();++i)if(c->fields[i].reference)f.references_closed &= i<f.elements.size() && f.elements[i].available && f.elements[i].address;
        }
        return f;
    }
    ObjectFact get(ValueId value,const Env& env)const{const auto it=env.values.find(value);return it==env.values.end()?ObjectFact{}:materialize(it->second,env);}
    void put(ValueId value,ObjectFact fact,Env& env,InstrRef at){if(!value)return;env.values.insert_or_assign(value,fact);out.scopes[at.block].changes[value].emplace_back(at.index+1,std::move(fact));}
    Type definition_type(ValueId value)const {
        const auto* record=uses.find(value);if(!record || record->definitions.size()!=1)return {};
        const auto at=record->definitions.front();return std::visit([](const auto& n)->Type {
            if constexpr(requires{n.type.kind;})return n.type;
            else if constexpr(requires{n.field_type;})return n.field_type;
            else if constexpr(requires{n.result.kind;})return n.result;
            else return {};
        },fn.blocks.at(at.block).instructions.at(at.index));
    }
    bool safe_array(ValueId value)const{
        const auto* record=uses.find(value);if(!record)return false;
        std::set<std::string> slots;std::optional<InstrRef> growth;
        for(const auto& use:record->uses)if(const auto* g=std::get_if<ArrayGrowMove>(&fn.blocks.at(use.at.block).instructions.at(use.at.index));g && g->array==value) {
            if(growth)return false;
            growth=use.at;
        }
        if(growth)for(const auto& use:record->uses)if(use.at!=*growth && !dom.dominates(use.at,*growth))return false;
        for(const auto& use:record->uses){if(!use.role_available)return false;
            const auto& instruction=fn.blocks.at(use.at.block).instructions.at(use.at.index);
            const bool safe=std::visit([&](const auto& node){using T=std::decay_t<decltype(node)>;
                if constexpr(std::is_same_v<T,ArrayLength> || std::is_same_v<T,ArrayGrowMove> || std::is_same_v<T,ArrayGet> || std::is_same_v<T,ArraySet> || std::is_same_v<T,ArrayInitializationComplete>)return node.array==value;
                else if constexpr(std::is_same_v<T,StoreLocal>){if(node.value!=value)return false;slots.insert(node.name);if(slots.size()!=1)return false;
                    for(const auto& b:fn.blocks)for(const auto& ins:b.instructions)if(const auto* address=std::get_if<AddressLocal>(&ins);address && address->name==node.name)return false;
                    return true;}
                else if constexpr(std::is_same_v<T,FieldSet>){
                    const auto* owner=semantic_source(use.at.block);
                    if(node.value!=value || !owner || !owner->complete || (!owner->rebind && owner->declaration.kind!=DeclarationIdentity::Kind::Constructor))return false;
                    // The binding analysis also checks the complete writer set.
                    const auto* c=layout(sources,definition_type(node.object));return c && c->complete && node.index<c->fields.size() && c->fields[node.index].is_private && !c->fields[node.index].reference && c->fields[node.index].type.kind==TypeKind::Array;
                } else return false;
            },instruction);
            if(!safe)return false;
        }
        return true;
    }
    Env external()const{
        Env env;
        for(std::size_t i=0;i<fn.parameters.size();++i){const auto& p=fn.parameters[i];ObjectFact f;f.type=p.type;f.available=true;f.initialized=!p.writable;
            if(p.writable && origin && origin->complete && origin->availability.validated_initialization) {
                const auto proof=origin->validated_initialization.find(static_cast<std::uint32_t>(i));
                if(proof!=origin->validated_initialization.end())f.initialized=std::find(proof->second.begin(),proof->second.end(),std::vector<semantics::PathStep>{})!=proof->second.end();
            }
            auto r=root(ObjectRoot::Parameter,{},p.type);r.parameter=static_cast<std::uint32_t>(i);f.sources.push_back({r,{}});f.references_closed=!references(layout(sources,p.type));
            env.locals.emplace(p.name,std::move(f));}
        return env;
    }
    Env entry(BlockId block)const{
        std::vector<const Env*> input;auto first=external();if(block==0)input.push_back(&first);
        for(const auto pred:cfg.pred[block])
            if(cfg.reachable[pred] && exits[pred])input.push_back(&*exits[pred]);
        if(input.empty())return {};
        auto env=*input.front();
        auto meet=[&](auto& map,const auto member){for(auto it=map.begin();it!=map.end();){bool same=true;
            for(const auto* e:input){const auto& other=e->*member;const auto f=other.find(it->first);if(f==other.end() || !(f->second==it->second)){same=false;break;}}
            if(!same)it=map.erase(it);else ++it;}};
        meet(env.values,&Env::values);meet(env.locals,&Env::locals);meet(env.references,&Env::references);meet(env.fields,&Env::fields);
        for(const auto* e:input)env.raw_bindings.insert(e->raw_bindings.begin(),e->raw_bindings.end());
        return env;
    }
    bool initialized(const ObjectPath& path)const{
        if(path.root.kind!=ObjectRoot::Parameter || !origin)return false;
        const auto found=origin->validated_initialization.find(path.root.parameter);return found!=origin->validated_initialization.end() && std::find(found->second.begin(),found->second.end(),path.steps)!=found->second.end();
    }
    ObjectFact field(ObjectFact object,std::size_t index,const Type& type,const Env& env)const{
        object=materialize(std::move(object),env);const auto* c=layout(sources,object.type);
        if(!c || !c->complete || index>=c->fields.size())return unavailable(type,BindingGap::MissingClass);
        ObjectFact f=index<object.elements.size()?materialize(object.elements[index],env):ObjectFact{};f.type=type;
        if(c->fields[index].reference){if(!f.available || !f.address)return unavailable(type,BindingGap::ReferenceUnavailable);f.address=false;return f;}
        f.sources.clear();for(auto path:object.sources){path.steps.push_back(semantics::PathStep::field_named(c->fields[index].name));f.sources.push_back(std::move(path));}
        f.available=object.available && !f.sources.empty();
        if(index>=object.elements.size()){f.initialized=!f.sources.empty() && std::all_of(f.sources.begin(),f.sources.end(),[&](const auto& p){return initialized(p);});f.references_closed=!references(layout(sources,type));}
        if(!f.initialized)gap(f,BindingGap::InitializationUnavailable);
        return f;
    }
    void change_field(ObjectFact object,std::size_t index,ObjectFact value,Env& env,InstrRef at,bool reference){
        out.writes.push_back({at,object,value,index,reference,!object.available,semantic_owner(at.block),semantic_scope(at.block)});
        if(!object.allocation || object.allocation->function!=fn.name)return;
        auto& fields=env.fields[{object.allocation->at.block,object.allocation->at.index}];
        if(index>=fields.size())fields.resize(index+1);
        fields[index]=std::move(value);
        for(auto& [id,fact]:env.values)if(fact.allocation==object.allocation){fact=materialize(fact,env);out.scopes[at.block].changes[id].emplace_back(at.index+1,fact);}
    }
    void invalidate(Env& env,InstrRef at,BindingGap why,bool expose){
        auto kill=[&](ObjectFact& f){if(f.type.kind!=TypeKind::Class && f.type.kind!=TypeKind::Array && !f.address)return;
            if(expose)out.exposures.push_back({at,f});
            f.available=false;f.initialized=false;f.array_closed=false;f.references_closed=false;f.elements.clear();gap(f,why);};
        for(auto& [id,f]:env.values){kill(f);out.scopes[at.block].changes[id].emplace_back(at.index+1,f);}
        for(auto& [name,f]:env.locals){(void)name;kill(f);}for(auto& [name,f]:env.references){(void)name;kill(f);}env.fields.clear();
    }
    void instruction(const Instruction& instruction,Env& env,InstrRef at){
        std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
            if constexpr(std::is_same_v<T,ConstantInt>){ObjectFact f;f.type=n.type;f.available=f.initialized=true;std::int64_t v{};const auto [end,error]=std::from_chars(n.value.data(),n.value.data()+n.value.size(),v);if(error==std::errc{} && end==n.value.data()+n.value.size())f.integer=v;put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,ConstantBool> || std::is_same_v<T,ConstantString>) {
                ObjectFact f;f.type=Type::simple(std::is_same_v<T,ConstantBool>?TypeKind::Bool:TypeKind::String);
                f.available=f.initialized=true;put(n.out,std::move(f),env,at);
            }
            else if constexpr(std::is_same_v<T,LoadLocal>){const auto found=env.locals.find(n.name);auto f=found==env.locals.end()?unavailable(n.type,BindingGap::Ambiguous):materialize(found->second,env);f.type=n.type;
                if(f.type.kind==TypeKind::Array && !safe_array(n.out)){f.array_closed=false;gap(f,BindingGap::ArrayAlias);}put(n.out,std::move(f),env,at);if(n.move_out)env.locals.erase(n.name);}
            else if constexpr(std::is_same_v<T,StoreLocal>){auto f=get(n.value,env);if(env.raw_bindings.contains(n.name)){
                    const auto old=env.locals.find(n.name);if(old!=env.locals.end())out.exposures.push_back({at,old->second});
                    f=unavailable(n.type,BindingGap::InlineOriginUnavailable);
                }
                // Storage ownership may be outer even in a nested return block.
                // Resolve the explicit renamed local first, never the block alone.
                if(origin && origin->availability.local_storage_paths) {
                    if(const auto path=origin->local_storage_paths.find(n.name);path!=origin->local_storage_paths.end())f.holding_paths.push_back(path->second);
                }
                if(const auto own=sources.scopes.find(fn.name);own!=sources.scopes.end())
                    for(const auto& [id,row]:own->second)if(row.complete && row.source.availability.local_storage_paths)
                        if(const auto path=row.source.local_storage_paths.find(n.name);path!=row.source.local_storage_paths.end())
                            f.scoped_holding_paths.push_back({fn.name,id,path->second});
                env.locals.insert_or_assign(n.name,std::move(f));}
            else if constexpr(std::is_same_v<T,ClassMake>){ObjectFact f;f.type=n.type;f.available=true;f.initialized=false;f.allocation=root(ObjectRoot::Allocation,at,n.type);f.allocation_identity_available=true;f.sources.push_back({*f.allocation,{}});
                const auto* c=layout(sources,n.type);auto& fields=env.fields[{at.block,at.index}];fields.assign(c?c->fields.size():n.fields.size(),{});
                for(std::size_t i=0;i<n.fields.size();++i)if(n.fields[i]){if(i>=fields.size())fields.resize(i+1);fields[i]=get(*n.fields[i],env);out.writes.push_back({at,f,fields[i],i,false,false,semantic_owner(at.block),semantic_scope(at.block)});}
                put(n.out,materialize(std::move(f),env),env,at);}
            else if constexpr(std::is_same_v<T,FieldGet>)put(n.out,field(get(n.object,env),n.index,n.field_type,env),env,at);
            else if constexpr(std::is_same_v<T,FieldSet>)change_field(get(n.object,env),n.index,get(n.value,env),env,at,false);
            else if constexpr(std::is_same_v<T,ArrayMake>){ObjectFact f;f.type=n.type;f.available=f.initialized=true;f.array_closed=safe_array(n.out);f.allocation=root(ObjectRoot::Allocation,at,n.type);for(const auto id:n.elements)f.elements.push_back(get(id,env));if(!f.array_closed)gap(f,BindingGap::ArrayAlias);put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,ArrayLength>){auto a=get(n.array,env);ObjectFact f;f.type=Type::simple(TypeKind::Int64);f.available=f.initialized=true;if(a.array_closed && a.elements.size()<=static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))f.integer=static_cast<std::int64_t>(a.elements.size());put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,ArrayGrowMove>){auto f=get(n.array,env);f.type=n.array_type;if(!f.array_closed || !safe_array(n.array) || !safe_array(n.out)){f.array_closed=false;gap(f,BindingGap::ArrayAlias);}else f.elements.emplace_back();f.allocation=root(ObjectRoot::Allocation,at,n.array_type);put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,ArraySet>){auto f=get(n.array,env);const auto i=get(n.index,env).integer;
                if(!i || *i<0 || static_cast<std::uint64_t>(*i)>=f.elements.size()){f.array_closed=false;gap(f,BindingGap::DynamicIndex);}else if(f.array_closed && safe_array(n.array))f.elements[static_cast<std::size_t>(*i)]=get(n.value,env);else {f.array_closed=false;gap(f,BindingGap::ArrayAlias);}put(n.array,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,ArrayGet>){const auto a=get(n.array,env),i=get(n.index,env);ObjectFact f=unavailable(n.element_type,BindingGap::DynamicIndex);
                if(a.array_closed && i.integer && *i.integer>=0 && static_cast<std::uint64_t>(*i.integer)<a.elements.size()) {
                    f=a.elements[static_cast<std::size_t>(*i.integer)];
                    if(!a.sources.empty()){f.sources=a.sources;for(auto& p:f.sources)p.steps.push_back(semantics::PathStep::element(*i.integer));}
                }put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,Clone> || std::is_same_v<T,Retain>){auto f=get(n.value,env);f.type=n.type;
                if(n.type.kind==TypeKind::Array){f.array_closed=false;gap(f,BindingGap::ArrayAlias);}else if constexpr(std::is_same_v<T,Clone>){if(n.type.kind==TypeKind::Class){f.allocation=root(ObjectRoot::Copy,at,n.type);f.allocation_identity_available=layout(sources,n.type) && layout(sources,n.type)->ordinary_value_layout;env.fields[{at.block,at.index}]=f.elements;}}
                put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,AddressLocal>){const auto f=env.locals.find(n.name);auto value=f==env.locals.end()?ObjectFact{}:materialize(f->second,env);value.address=true;put(n.out,std::move(value),env,at);}
            else if constexpr(std::is_same_v<T,AddressField>){auto object=get(n.object,env);const auto* c=layout(sources,object.type);auto f=c && n.index<c->fields.size()?field(object,n.index,c->fields[n.index].type,env):ObjectFact{};f.address=true;put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,InlineBindAddress>){auto f=get(n.address,env);f.address=false;f.type=n.type;env.locals.insert_or_assign(n.name,std::move(f));env.raw_bindings.insert(n.name);}
            else if constexpr(std::is_same_v<T,BindReference>)env.references[n.name]=get(n.address,env);
            else if constexpr(std::is_same_v<T,ReferenceAddress>){const auto r=env.references.find(n.name);put(n.out,r==env.references.end()?ObjectFact{}:r->second,env,at);}
            else if constexpr(std::is_same_v<T,LoadAddress> || std::is_same_v<T,LoadReference>){ObjectFact f;if constexpr(std::is_same_v<T,LoadAddress>)f=get(n.address,env);else if(const auto r=env.references.find(n.name);r!=env.references.end())f=r->second;f.type=n.type;f.address=false;put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,BindFieldReference>)change_field(get(n.object,env),n.index,get(n.address,env),env,at,true);
            else if constexpr(std::is_same_v<T,FieldReferenceAddress>){auto object=get(n.object,env);auto f=n.index<object.elements.size()?object.elements[n.index]:ObjectFact{};if(!f.address)f=unavailable({},BindingGap::ReferenceUnavailable);put(n.out,std::move(f),env,at);}
            else if constexpr(std::is_same_v<T,Call>){const auto* callee=function_source(sources,n.callee);const auto* summary=summaries.find(n.callee);
                const bool exact=summary && summary->semantic && summary->effects_complete && !(summary->effects & effect_bit::Opaque);
                for(const auto& arg:n.args){auto f=get(arg.writable_address.value_or(arg.value),env);if(!exact && (f.type.kind==TypeKind::Class || f.type.kind==TypeKind::Array || f.address))out.exposures.push_back({at,f});}
                auto f=unavailable(n.result,BindingGap::UnknownCall);
                if(callee && callee->complete && callee->declaration.kind==DeclarationIdentity::Kind::Constructor && n.result.kind==TypeKind::Class){f.type=n.result;f.available=f.initialized=true;f.gaps.clear();f.allocation=root(ObjectRoot::ConstructorCall,at,n.result);f.allocation->constructor_declaration=declaration_key(callee->declaration);f.allocation->constructor_function=n.callee;f.allocation->constructor_arguments=callee->declaration.generic_args;f.allocation_identity_available=callee->constructor_allocation_proven;f.sources.push_back({*f.allocation,{}});for(const auto& arg:n.args)f.call_arguments.push_back(get(arg.writable_address.value_or(arg.value),env));}
                const bool mutates=exact && (summary->semantic->mutates_ambient || summary->semantic->mutates_receiver ||
                    std::any_of(summary->semantic->exposed_writes.begin(),summary->semantic->exposed_writes.end(),[](const auto& p){return !p.empty();}) ||
                    std::any_of(summary->semantic->mutates_shared_of_parameter.begin(),summary->semantic->mutates_shared_of_parameter.end(),[](bool b){return b;}) ||
                    !summary->semantic->reference_shared_writes.empty() || !summary->semantic->reference_shared_write_origins.empty());
                if(!exact || mutates)invalidate(env,at,BindingGap::UnknownCall,!exact || summary->semantic->mutates_ambient);
                // A result born after the call is not invalidated with older state.
                put(n.out,std::move(f),env,at);
            } else if constexpr(std::is_same_v<T,IndirectCall> || std::is_same_v<T,ParallelBlock>)invalidate(env,at,BindingGap::UnknownCall,true);
            else if constexpr(std::is_same_v<T,StoreAddress> || std::is_same_v<T,StoreReference> || std::is_same_v<T,Pin>){ObjectFact f;if constexpr(std::is_same_v<T,StoreAddress>)f=get(n.address,env);else if constexpr(std::is_same_v<T,Pin>)f=get(n.value,env);else if(const auto r=env.references.find(n.name);r!=env.references.end())f=r->second;out.exposures.push_back({at,f});invalidate(env,at,BindingGap::ReferenceUnavailable,false);}
            else if constexpr(std::is_same_v<T,Return>)out.returns.push_back(get(n.value,env));
            else if constexpr(std::is_same_v<T,ArraySliceSet> || std::is_same_v<T,ArraySorted>){const auto f=get(n.array,env);out.exposures.push_back({at,f});}
        },instruction);
    }
    ObjectProvenance run(){bool changed=true;while(changed){changed=false;out.writes.clear();out.exposures.clear();out.returns.clear();
        for(const auto b:cfg.rpo){auto env=entry(b);auto& scope=out.scopes[b];scope.incoming=env.values;scope.changes.clear();scope.instruction_count=static_cast<std::uint32_t>(fn.blocks[b].instructions.size());
            for(std::uint32_t i=0;i<scope.instruction_count;++i)instruction(fn.blocks[b].instructions[i],env,{b,i});
            if(!exits[b] || *exits[b]!=env){exits[b]=std::move(env);changed=true;}}
    }return std::move(out);}
};
} // namespace
bool ObjectFact::operator==(const ObjectFact& o)const{return type==o.type && type.length==o.type.length && available==o.available && initialized==o.initialized && array_closed==o.array_closed && address==o.address && references_closed==o.references_closed && allocation_identity_available==o.allocation_identity_available && sources==o.sources && allocation==o.allocation && holding_paths==o.holding_paths && elements==o.elements && call_arguments==o.call_arguments && integer==o.integer && gaps==o.gaps && scoped_holding_paths==o.scoped_holding_paths;}
ObjectFact ObjectProvenance::at(InstrRef site,ValueId value)const{
    if(site.block>=scopes.size() || !reachable[site.block] || site.index>scopes[site.block].instruction_count)return unavailable({},BindingGap::Unreachable);
    const auto d=definitions.find(value);if(d==definitions.end() || !dominance.dominates(d->second,site) || d->second==site)return unavailable({},BindingGap::Ambiguous);
    const auto& s=scopes[site.block];if(const auto c=s.changes.find(value);c!=s.changes.end()){const auto end=std::upper_bound(c->second.begin(),c->second.end(),site.index,[](auto i,const auto& p){return i<p.first;});if(end!=c->second.begin())return std::prev(end)->second;}
    const auto found=s.incoming.find(value);return found==s.incoming.end()?unavailable({},BindingGap::Ambiguous):found->second;
}
ObjectProvenance object_provenance(const Function& f,const Cfg& cfg,const DomTree& dom,const LocalSsa&,const Slots&,const DefUse& uses,const ModuleSummaries& summaries,const BindingSources& sources){return Engine(f,cfg,dom,uses,summaries,sources).run();}
BindingSources adapt_binding_sources(const Module& module,const ModuleSummaries& summaries,const semantics::InterfaceSources& interfaces,const BindingOriginInputs& input){
    BindingSources result;
    const auto issue=[&](BindingSourceComponent component,BindingGap reason,const std::string& lookup){
        result.issues.push_back({component,reason,lookup});
    };
    for(const auto& layout:module.classes){
        bool found=false;
        for(const auto& original:interfaces.modules){
            const auto n=original.contracts.named_types.find(layout.name);
            if(n==original.contracts.named_types.end())continue;
            const auto& identity=n->second;
            const auto c=std::find_if(original.contracts.classes.begin(),original.contracts.classes.end(),[&](const auto& value){return value.source_name==identity.source_name;});
            if(c==original.contracts.classes.end())continue;
            found=true;
            BindingClass value;value.identity={identity.module.package,identity.module.module_path,identity.source_name,identity.generic_arguments};
            const auto instance=c->instances.find(identity.generic_arguments);
            const auto& fields=instance==c->instances.end()?c->fields:instance->second.fields;
            value.ordinary_value_layout=!layout.standard_library;
            value.complete=identity.module==original.module && original.contracts.classes_complete && original.contracts.declarations_complete && fields.size()<=layout.fields.size() && fields.size()<=layout.field_names.size();
            for(std::size_t i=0;i<fields.size();++i){
                value.complete &= i<layout.field_names.size() && fields[i].name==layout.field_names[i] && i<layout.fields.size() && fields[i].type.semantic.has_value();
                if(i<layout.fields.size() && fields[i].type.semantic)
                    value.complete &= *fields[i].type.semantic==layout.fields[i] && fields[i].reference==layout.reference_fields.contains(i);
                value.fields.push_back({fields[i].name,i<layout.fields.size()?layout.fields[i]:Type{},fields[i].is_const,fields[i].is_private,fields[i].reference});
            }
            if(!c->generic_parameters.empty() && instance==c->instances.end())value.complete=false;
            if(!value.complete)issue(BindingSourceComponent::ClassLayout,BindingGap::MissingClass,layout.name);
            const auto existing=result.classes.find(layout.name);
            if(existing==result.classes.end())result.classes.emplace(layout.name,std::move(value));
            else {
                const bool same=existing->second.identity==value.identity && existing->second.fields.size()==value.fields.size() &&
                    std::equal(existing->second.fields.begin(),existing->second.fields.end(),value.fields.begin(),[](const auto& a,const auto& b){
                        return a.name==b.name && a.type==b.type && a.is_const==b.is_const && a.is_private==b.is_private && a.reference==b.reference;
                    });
                existing->second.complete &= value.complete && same;
                if(!same)issue(BindingSourceComponent::ClassLayout,BindingGap::Ambiguous,layout.name);
            }
        }
        if(!found)issue(BindingSourceComponent::ClassLayout,BindingGap::MissingClass,layout.name);
    }
    for(const auto& fn:module.functions){
        FunctionBindingSource origin;
        if(fn.declaration)origin.declaration=*fn.declaration;
        std::size_t declarations=0;
        if(fn.declaration)for(const auto& original:interfaces.modules)if(original.contracts.declarations_complete)
            for(const auto& declaration:original.contracts.functions)
                if(same_declaration(declaration.declaration,*fn.declaration))++declarations;
        origin.complete=declarations==1;
        if(!origin.complete)issue(BindingSourceComponent::Declaration,declarations>1?BindingGap::Ambiguous:BindingGap::MissingOrigin,fn.name);
        const auto* summary=summaries.find(fn.name);
        const auto owned=summary?summaries.semantic_snapshots().find(summary->semantic_key):summaries.semantic_snapshots().end();
        const bool exact_summary=summary && summary->semantic && owned!=summaries.semantic_snapshots().end() && summary->semantic==&owned->second;
        if(exact_summary){
            origin.semantic_parameter_for_ir_parameter=summary->semantic_parameter_for_ir_parameter;
            origin.availability.parameter_map=origin.semantic_parameter_for_ir_parameter.size()==fn.parameters.size() &&
                std::all_of(origin.semantic_parameter_for_ir_parameter.begin(),origin.semantic_parameter_for_ir_parameter.end(),[&](const auto& index){return !index || *index<summary->semantic->exposed_reads.size();});
        }
        origin.complete &= exact_summary && origin.availability.parameter_map;
        if(!exact_summary)issue(BindingSourceComponent::ParameterMap,BindingGap::MissingEffect,fn.name);
        else if(!origin.availability.parameter_map)issue(BindingSourceComponent::ParameterMap,BindingGap::Ambiguous,fn.name);
        if(fn.declaration && fn.declaration->kind==DeclarationIdentity::Kind::Method){
            std::size_t receivers=0;
            if(origin.availability.parameter_map)for(std::size_t i=0;i<origin.semantic_parameter_for_ir_parameter.size();++i)
                if(origin.semantic_parameter_for_ir_parameter[i]==0){++receivers;origin.receiver_ir_parameter=static_cast<std::uint32_t>(i);}
            origin.availability.receiver=receivers==1;
            if(!origin.availability.receiver){origin.receiver_ir_parameter.reset();issue(BindingSourceComponent::Receiver,receivers?BindingGap::Ambiguous:BindingGap::MissingOrigin,fn.name);}
            origin.complete &= origin.availability.receiver;
        }
        const auto supplied=input.functions.find(fn.name);
        const bool exact_origin=fn.declaration && origin.complete && supplied!=input.functions.end() && supplied->second.complete &&
            same_declaration(supplied->second.declaration,*fn.declaration) && supplied->second.declaration.generic_args==fn.declaration->generic_args &&
            supplied->second.semantic_parameter_for_ir_parameter==origin.semantic_parameter_for_ir_parameter &&
            supplied->second.receiver_ir_parameter==origin.receiver_ir_parameter;
        if(exact_origin){
            const auto& source=supplied->second;
            origin.availability.validated_initialization=source.availability.validated_initialization;
            origin.availability.local_storage_paths=source.availability.local_storage_paths;
            origin.availability.collections=source.availability.collections;
            origin.availability.rebind_role=source.availability.rebind_role;
            origin.availability.rebind_events=source.availability.rebind_events;
            if(origin.availability.rebind_events)origin.rebind_events=source.rebind_events;
            origin.availability.traversals=source.availability.traversals;
            if(origin.availability.traversals)origin.traversals=source.traversals;
            if(origin.availability.validated_initialization)origin.validated_initialization=source.validated_initialization;
            if(origin.availability.local_storage_paths)origin.local_storage_paths=source.local_storage_paths;
            if(origin.availability.collections)origin.collections=source.collections;
            if(origin.availability.rebind_role)origin.rebind=source.rebind;
        }
        // This set is an explicitly captured positive role, not a spelling
        // heuristic. Its absence never proves an ordinary/non-rebind origin.
        if(fn.declaration && origin.complete && input.rebind_declarations.contains(declaration_key(*fn.declaration))){
            origin.availability.rebind_role=true;origin.rebind=true;
        }
        if(origin.rebind && (!fn.declaration || fn.declaration->kind!=DeclarationIdentity::Kind::Method || !fn.declaration->mutation_marker)){
            origin.rebind=false;origin.availability.rebind_role=false;
        }
        if(!origin.availability.validated_initialization)issue(BindingSourceComponent::ValidatedInitialization,BindingGap::MissingOrigin,fn.name);
        if(!origin.availability.local_storage_paths)issue(BindingSourceComponent::LocalStoragePaths,BindingGap::MissingOrigin,fn.name);
        if(!origin.availability.collections)issue(BindingSourceComponent::Collections,BindingGap::MissingOrigin,fn.name);
        if(!origin.availability.rebind_role)issue(BindingSourceComponent::RebindRole,BindingGap::MissingOrigin,fn.name);
        if(!origin.availability.rebind_events)issue(BindingSourceComponent::RebindEvents,BindingGap::MissingOrigin,fn.name);
        if(!origin.availability.traversals)issue(BindingSourceComponent::Traversals,BindingGap::MissingOrigin,fn.name);
        // Allocation birth is proved from the current body by binding_proof,
        // never imported from a constructor role or a stale source boolean.
        if(!result.functions.emplace(fn.name,std::move(origin)).second){
            auto& duplicate=result.functions.at(fn.name);duplicate.complete=false;duplicate.availability={};
            duplicate.rebind=false;duplicate.receiver_ir_parameter.reset();duplicate.validated_initialization.clear();
            duplicate.local_storage_paths.clear();duplicate.collections.clear();
            issue(BindingSourceComponent::Declaration,BindingGap::Ambiguous,fn.name);
        }
    }
    // Rows were checked against the actual scope/bind instructions by the
    // owned origin reader. Keep each original semantic namespace distinct.
    for(const auto& [host,rows]:input.scopes) {
        const auto owner=std::find_if(module.functions.begin(),module.functions.end(),[&](const auto& f){return f.name==host;});
        if(owner==module.functions.end())continue;
        for(const auto& [id,row]:rows) {
            if(id==row.scope && valid_scoped_binding_source(module,*owner,row))result.scopes[host].emplace(id,row);
            else issue(BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable,host);
        }
    }
    return result;
}
} // namespace quidra::ir::analysis
