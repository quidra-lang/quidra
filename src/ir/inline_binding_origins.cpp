#include "ir/inline_binding_origins.hpp"
#include "ir/summaries.hpp"
#include "ir/mutable_operands.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace quidra::ir::analysis {
namespace {
struct Missing : std::runtime_error {using std::runtime_error::runtime_error;};
InstrRef site(const InlineOriginMap& map,InstrRef at,bool boundary) {
    if(boundary) {
        const auto found=map.boundaries.find(at);
        if(found!=map.boundaries.end())return found->second;
    } else {
        const auto found=map.instructions.find(at);
        if(found!=map.instructions.end() && found->second)return *found->second;
    }
    throw Missing("inline origin site is unmapped");
}
ValueId value(const InlineOriginMap& map,ValueId id) {
    if(!id)return 0;
    const auto found=map.values.find(id);
    if(found==map.values.end() || !found->second)throw Missing("inline origin SSA value is unmapped");
    return found->second;
}
std::string local(const InlineOriginMap& map,const std::string& name) {
    const auto found=map.locals.find(name);
    if(found==map.locals.end())throw Missing("inline origin storage is unmapped");
    return found->second;
}
std::uint64_t scope(const InlineOriginMap& map,std::uint64_t id) {
    if(!id)return 0;
    const auto found=map.scopes.find(id);
    if(found==map.scopes.end() || !found->second)throw Missing("inline origin scope is unmapped");
    return found->second;
}
void valid_site(const Function& fn,InstrRef at,bool boundary) {
    if(at.block>=fn.blocks.size() || at.index>fn.blocks[at.block].instructions.size() ||
       (!boundary && at.index==fn.blocks[at.block].instructions.size()))
        throw Missing("inline origin target site is out of range");
}
FunctionBindingSource relocated(FunctionBindingSource row,const InlineOriginMap& map,
    const std::string& host,std::vector<BindingSourceIssue>& issues) {
    const auto component=[&](bool& available,BindingSourceComponent kind,const auto& work) {
        if(!available)return;
        try {work();}catch(const Missing&) {available=false;issues.push_back({kind,BindingGap::InlineOriginUnavailable,host});}
    };
    component(row.availability.local_storage_paths,BindingSourceComponent::LocalStoragePaths,[&] {
        decltype(row.local_storage_paths) paths;
        for(const auto& [name,path]:row.local_storage_paths)
            if(!paths.emplace(local(map,name),path).second)throw Missing("inline holder mapping collides");
        row.local_storage_paths=std::move(paths);
    });
    component(row.availability.collections,BindingSourceComponent::Collections,[&] {
        for(auto& query:row.collections) {query.at=site(map,query.at,true);query.root=value(map,query.root);query.array=value(map,query.array);}
    });
    component(row.availability.traversals,BindingSourceComponent::Traversals,[&] {
        for(auto& query:row.traversals) {
            query.at=site(map,query.at,true);query.root=value(map,query.root);
            if(query.output)query.output=value(map,*query.output);
            if(query.root_address)query.root_address=value(map,*query.root_address);
        }
    });
    component(row.availability.rebind_events,BindingSourceComponent::RebindEvents,[&] {
        for(auto& event:row.rebind_events) {
            event.at=site(map,event.at,false);event.address=value(map,event.address);
            if(event.kind==BindingRebindEvent::Kind::Field)event.object=value(map,event.object);
            else event.local=local(map,event.local);
        }
    });
    if(!row.availability.local_storage_paths)row.local_storage_paths.clear();
    if(!row.availability.collections)row.collections.clear();
    if(!row.availability.traversals)row.traversals.clear();
    if(!row.availability.rebind_events)row.rebind_events.clear();
    return row;
}
ScopedBindingSource relocated(ScopedBindingSource row,const InlineOriginMap& map,
    const std::string& host,std::vector<BindingSourceIssue>& issues) {
    row.scope=scope(map,row.scope);row.parent=scope(map,row.parent);row.begin=site(map,row.begin,false);
    row.source=relocated(std::move(row.source),map,host,issues);
    for(auto& binding:row.parameters) {
        binding.storage=local(map,binding.storage);binding.at=site(map,binding.at,false);
        binding.value=value(map,binding.value);
        if(binding.address)binding.address=value(map,*binding.address);
    }
    for(auto& returned:row.returns) {
        returned.value=value(map,returned.value);returned.output=value(map,returned.output);
        if(returned.storage)returned.storage=local(map,*returned.storage);
        if(returned.store)returned.store=site(map,*returned.store,false);
        if(returned.load)returned.load=site(map,*returned.load,false);
        returned.cleanup=site(map,returned.cleanup,false);
        returned.end=site(map,returned.end,false);returned.jump=site(map,returned.jump,false);
        const auto continuation=map.labels.find(returned.continuation);
        if(continuation==map.labels.end())throw Missing("inline return continuation is unmapped");
        returned.continuation=continuation->second;
    }
    return row;
}
struct CheckRename final:MutableOperandVisitor {
    const InlineOriginMap& map;
    explicit CheckRename(const InlineOriginMap& value):map(value) {}
    void definition(ValueId& id)override{id=value(map,id);}
    void use(ValueId& id)override{id=value(map,id);}
    void metadata_value(ValueId& id)override{id=value(map,id);}
    void local(std::string& name,LocalRole)override{name=analysis::local(map,name);}
    void successor(std::string& name)override{
        const auto found=map.labels.find(name);
        if(found==map.labels.end())throw Missing("inline successor is unmapped");
        name=found->second;
    }
};
void validate_map(const Function& before,const Function& after,const InlineOriginMap& map,
    std::optional<InstrRef> removed_call,bool copied) {
    CheckRename rename(map);
    std::set<InstrRef,InlineSiteLess> targets;
    for(std::size_t b=0;b<before.blocks.size();++b) {
        if(b>std::numeric_limits<BlockId>::max() || before.blocks[b].instructions.size()>std::numeric_limits<std::uint32_t>::max())
            throw Missing("inline source dimensions exceed site representation");
        const auto block=static_cast<BlockId>(b);
        for(std::uint32_t i=0;i<=before.blocks[b].instructions.size();++i) {
            const InstrRef old{block,i};valid_site(after,site(map,old,true),true);
            if(i==before.blocks[b].instructions.size())break;
            const auto found=map.instructions.find(old);
            if(found==map.instructions.end())throw Missing("inline instruction disposition is missing");
            const auto& original=before.blocks[b].instructions[i];
            if(!found->second) {
                if(removed_call && old==*removed_call)continue;
                if(copied && (std::holds_alternative<Return>(original) || std::holds_alternative<ReturnVoid>(original) || std::holds_alternative<Exit>(original)))continue;
                throw Missing("inline removed a source instruction without a typed disposition");
            }
            valid_site(after,*found->second,false);
            if(!targets.insert(*found->second).second)throw Missing("inline instruction mapping collides");
            auto expected=original;mutate_operands(expected,rename);
            if(copied) {
                if(auto* n=std::get_if<StatementBoundary>(&expected)) {
                    const auto id=map.statements.find(n->identity);
                    if(id==map.statements.end())throw Missing("inline statement is unmapped");
                    n->identity=id->second;
                }
                if(auto* n=std::get_if<InlineScopeBegin>(&expected))n->scope=scope(map,n->scope);
                if(auto* n=std::get_if<InlineScopeCleanup>(&expected))n->scope=scope(map,n->scope);
                if(auto* n=std::get_if<InlineScopeEnd>(&expected))n->scope=scope(map,n->scope);
                if(auto* n=std::get_if<InlineExit>(&expected))n->scope=scope(map,n->scope);
            }
            if(canonical_encode_value(expected).bytes!=canonical_encode_value(after.blocks[found->second->block].instructions[found->second->index]).bytes)
                throw Missing("inline instruction receipt disagrees with actual emitted fields");
        }
    }
}
bool same_type(const Type& a,const Type& b) {
    return canonical_encode_value(a).bytes==canonical_encode_value(b).bytes;
}
void valid_return_binding(const Function& fn,const ScopedBindingSource& row,const InlineReturnBinding& returned) {
    valid_site(fn,returned.cleanup,false);valid_site(fn,returned.end,false);valid_site(fn,returned.jump,false);
    if(returned.cleanup.block!=returned.end.block || returned.end.block!=returned.jump.block ||
       std::uint64_t(returned.end.index)!=std::uint64_t(returned.cleanup.index)+1 ||
       std::uint64_t(returned.jump.index)!=std::uint64_t(returned.end.index)+1)
        throw Missing("inline return cleanup/end/jump order is invalid");
    const auto* owner=inline_binding_scope(fn,returned.cleanup.block);
    if(!owner || owner->identity!=row.scope)throw Missing("inline return crosses its owning scope");
    const auto& code=fn.blocks[returned.cleanup.block].instructions;
    const auto* cleanup=std::get_if<InlineScopeCleanup>(&code[returned.cleanup.index]);
    const auto* end=std::get_if<InlineScopeEnd>(&code[returned.end.index]);
    const auto* jump=std::get_if<Jump>(&code[returned.jump.index]);
    if(!cleanup || cleanup->scope!=row.scope || !end || end->scope!=row.scope || !jump || jump->target!=returned.continuation ||
       !same_type(returned.type,row.result_type))throw Missing("inline return actual scope/result fields disagree");
    const auto continuation=std::find_if(fn.blocks.begin(),fn.blocks.end(),[&](const auto& b){return b.label==returned.continuation;});
    if(continuation==fn.blocks.end())throw Missing("inline return continuation is missing");
    const auto continuation_id=static_cast<BlockId>(continuation-fn.blocks.begin());
    const auto* resumed=inline_binding_scope(fn,continuation_id);
    if((resumed?resumed->identity:0)!=row.parent)throw Missing("inline return continuation leaves its parent context");
    if(returned.kind==InlineReturnBinding::Kind::Void) {
        if(row.result_type.kind!=TypeKind::Void || bool(returned.original_value)!=bool(returned.value) || returned.output ||
           returned.storage || returned.store || returned.load || returned.store_borrowed ||
           returned.store_replace_without_release || returned.load_move_out)
            throw Missing("inline void return contains a value disposition");
        return;
    }
    if(returned.kind!=InlineReturnBinding::Kind::Value || row.result_type.kind==TypeKind::Void ||
       row.result_type.kind==TypeKind::Never || !returned.original_value || !returned.value || !returned.output ||
       !returned.storage || !returned.store || !returned.load || returned.store_borrowed ||
       returned.store_replace_without_release || returned.load_move_out!=owns_managed(row.result_type))
        throw Missing("inline value return has an unavailable result ABI");
    valid_site(fn,*returned.store,false);valid_site(fn,*returned.load,false);
    if(returned.store->block!=returned.cleanup.block ||
       std::uint64_t(returned.cleanup.index)!=std::uint64_t(returned.store->index)+1 || returned.load->block!=continuation_id)
        throw Missing("inline return store/load disposition is invalid");
    const auto* store=std::get_if<StoreLocal>(&fn.blocks[returned.store->block].instructions[returned.store->index]);
    const auto* load=std::get_if<LoadLocal>(&fn.blocks[returned.load->block].instructions[returned.load->index]);
    if(!store || store->name!=*returned.storage || store->value!=returned.value || !same_type(store->type,returned.type) ||
       store->borrowed!=returned.store_borrowed || store->replace_without_release!=returned.store_replace_without_release ||
       !load || load->name!=*returned.storage || load->out!=returned.output || !same_type(load->type,returned.type) ||
       load->move_out!=returned.load_move_out)
        throw Missing("inline return actual storage/SSA/type/ownership fields disagree");
    // Result storage belongs to the parent, not to the frame being cleaned.
    bool declaration=false;
    for(const auto& block:fn.blocks)for(const auto& ins:block.instructions)
        if(const auto* local=std::get_if<DeclareLocal>(&ins);local && local->name==*returned.storage && same_type(local->type,returned.type))declaration=true;
    if(!declaration)throw Missing("inline result storage has no actual declaration");
    if(row.parent) {
        const auto parent=std::find_if(fn.inline_scopes.begin(),fn.inline_scopes.end(),[&](const auto& scope){return scope.identity==row.parent;});
        if(parent==fn.inline_scopes.end() || std::none_of(parent->slots.begin(),parent->slots.end(),[&](const auto& slot){
            return slot.name==*returned.storage && slot.kind==InlineSlotKind::OwnedLocal && same_type(slot.type,returned.type);
        }))throw Missing("inline result storage lacks its exact parent slot");
    }
    for(const auto& frame:fn.inline_scopes)
        if(frame.identity!=row.parent && std::any_of(frame.slots.begin(),frame.slots.end(),[&](const auto& slot){return slot.name==*returned.storage;}))
            throw Missing("inline result storage is owned by another frame");
}
void validate_returns(const Function& before,const Function& after,const InlineOriginTransfer& transfer) {
    std::map<InstrRef,const InlineReturnBinding*,InlineSiteLess> returned;
    for(const auto& row:transfer.returns)
        if(!returned.emplace(row.original,&row).second)throw Missing("duplicate original return disposition");
    ScopedBindingSource scope;scope.scope=transfer.scope;scope.parent=transfer.parent;scope.result_type=before.result;
    std::size_t count=0;
    for(std::size_t b=0;b<before.blocks.size();++b)for(std::uint32_t i=0;i<before.blocks[b].instructions.size();++i) {
        const auto& instruction=before.blocks[b].instructions[i];
        const auto* result=std::get_if<Return>(&instruction);
        const bool empty=std::holds_alternative<ReturnVoid>(instruction);
        if(!result && !empty)continue;
        ++count;const InstrRef original{static_cast<BlockId>(b),i};
        const auto receipt=returned.find(original);if(receipt==returned.end())throw Missing("original return disposition is missing");
        const auto& row=*receipt->second;
        if(result) {
            const auto kind=result->type.kind==TypeKind::Void?InlineReturnBinding::Kind::Void:InlineReturnBinding::Kind::Value;
            if(row.kind!=kind || row.original_value!=result->value ||
               row.value!=value(transfer.copied,result->value) || !same_type(row.type,result->type))
                throw Missing("original typed value return disagrees with its receipt");
        } else if(row.kind!=InlineReturnBinding::Kind::Void || row.original_value || !same_type(row.type,before.result))
            throw Missing("original void return disagrees with its receipt");
        if(row.output!=transfer.removed_call.out)throw Missing("inline return output differs from the actual removed call");
        valid_return_binding(after,scope,row);
        const auto first=row.store.value_or(row.cleanup);
        const auto begin=site(transfer.copied,original,true),end=site(transfer.copied,{original.block,i+1},true);
        if(first!=begin || end.block!=row.jump.block || std::uint64_t(end.index)!=std::uint64_t(row.jump.index)+1)
            throw Missing("inline return expansion does not match the original instruction boundaries");
    }
    if(count!=returned.size())throw Missing("return receipt names a non-return instruction");
}
const BindingOriginSnapshot* fresh(const Module& module,const Function& fn) {
    if(!module.binding_origins)return nullptr;
    const auto found=module.binding_origins->functions.find(fn.name);
    if(found==module.binding_origins->functions.end() || found->second.body_fields!=binding_origin_body_fields(fn))return nullptr;
    const auto& row=found->second.source;
    if(found->second.root_source_available) {
        if(!fn.declaration || !same_declaration(row.declaration,*fn.declaration) || row.declaration.generic_args!=fn.declaration->generic_args)return nullptr;
        const auto* summary=module.summaries?module.summaries->find(fn.name):nullptr;
        if(!summary || row.semantic_parameter_for_ir_parameter!=summary->semantic_parameter_for_ir_parameter)return nullptr;
    }
    return &found->second;
}
} // namespace
InlineOriginResult transfer_inline_origins(const Module& module,const Function& caller,
    const Function& callee,const Function& expanded,const InlineOriginTransfer& transfer) {
    InlineOriginResult result;
    if(!module.binding_origins)return result;
    auto snapshots=std::make_shared<BindingOriginSnapshots>(*module.binding_origins);
    const auto unavailable=[&](BindingSourceComponent component,BindingGap reason) {
        result.unavailable.push_back({component,reason,caller.name});
    };
    try {
        if(transfer.caller!=caller.name || transfer.callee!=callee.name || expanded.name!=caller.name ||
           transfer.caller_body!=binding_origin_body_fields(caller) || transfer.callee_body!=binding_origin_body_fields(callee) ||
           snapshots->class_fields!=canonical_encode_value(module.classes).bytes)
            throw Missing("inline origin preimage is stale");
        valid_site(caller,transfer.call,false);
        const auto* actual=std::get_if<Call>(&caller.blocks[transfer.call.block].instructions[transfer.call.index]);
        if(!actual || actual->callee!=callee.name || canonical_encode_value(*actual).bytes!=canonical_encode_value(transfer.removed_call).bytes)
            throw Missing("inline origin removed call is not the actual call");
        validate_map(caller,expanded,transfer.retained,transfer.call,false);
        validate_map(callee,expanded,transfer.copied,{},true);
        validate_returns(callee,expanded,transfer);
        const auto* original=fresh(module,caller);
        BindingOriginSnapshot output;
        if(original)output=*original;
        else {
            output.root_source_available=false;
            output.unavailable.push_back({BindingSourceComponent::Body,BindingGap::MissingOrigin,caller.name});
        }
        if(output.root_source_available)output.source=relocated(output.source,transfer.retained,caller.name,output.unavailable);
        output.scopes.clear();
        if(original)for(const auto& [id,row]:original->scopes) {
            (void)id;
            try {auto copy=relocated(row,transfer.retained,caller.name,output.unavailable);output.scopes.emplace(copy.scope,std::move(copy));}
            catch(const Missing&){unavailable(BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);}
        }
        const auto* source=fresh(module,callee);
        if(source && source->root_source_available) {
            ScopedBindingSource root;root.scope=transfer.scope;root.parent=transfer.parent;root.source_function=callee.name;
            root.begin=transfer.begin;root.parameters=transfer.parameters;
            root.result_type=callee.result;root.returns_complete=true;root.returns=transfer.returns;
            root.source=relocated(source->source,transfer.copied,caller.name,output.unavailable);
            const auto* summary=module.summaries?module.summaries->find(callee.name):nullptr;
            if(summary && summary->semantic && module.summaries->semantic_snapshots().contains(summary->semantic_key) &&
               &module.summaries->semantic_snapshots().at(summary->semantic_key)==summary->semantic) {
                root.semantic_key=summary->semantic_key;root.semantic=*summary->semantic;
                root.complete=root.source.complete;
            }
            if(root.parameters.size()!=callee.parameters.size())root.complete=false;
            for(std::size_t p=0;p<root.parameters.size();++p) {
                auto& binding=root.parameters[p];
                if(p>=callee.parameters.size() || p>=root.source.semantic_parameter_for_ir_parameter.size()) {root.complete=false;continue;}
                binding.semantic_parameter=root.source.semantic_parameter_for_ir_parameter[p];
                const auto& formal=callee.parameters[p];const auto& arg=actual->args.at(p);
                if(binding.ir_parameter!=p || binding.storage!=local(transfer.copied,formal.name) || binding.type!=formal.type ||
                   binding.writable!=formal.writable || binding.borrowed!=formal.borrowed ||
                   binding.value!=arg.value || binding.address!=arg.writable_address)root.complete=false;
                valid_site(expanded,binding.at,false);
                const auto& ins=expanded.blocks[binding.at.block].instructions[binding.at.index];
                if(formal.writable) {
                    const auto* bind=std::get_if<InlineBindAddress>(&ins);
                    if(!bind || bind->name!=binding.storage || bind->address!=binding.address.value_or(0) || bind->type!=formal.type)root.complete=false;
                } else {
                    const auto* bind=std::get_if<StoreLocal>(&ins);
                    if(!bind || bind->name!=binding.storage || bind->value!=binding.value || bind->type!=formal.type || bind->borrowed!=formal.borrowed)root.complete=false;
                }
            }
            if(!root.complete)output.unavailable.push_back({BindingSourceComponent::ParameterMap,BindingGap::InlineOriginUnavailable,caller.name});
            if(!output.scopes.emplace(root.scope,std::move(root)).second)throw Missing("inline root scope collides");
        } else output.unavailable.push_back({BindingSourceComponent::Body,BindingGap::MissingOrigin,caller.name});
        if(source)for(const auto& [id,row]:source->scopes) {
            (void)id;
            try {auto copy=relocated(row,transfer.copied,caller.name,output.unavailable);if(!copy.parent)copy.parent=transfer.scope;
                if(!output.scopes.emplace(copy.scope,std::move(copy)).second)throw Missing("inline nested scope collides");}
            catch(const Missing&){output.unavailable.push_back({BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable,caller.name});}
        }
        output.unavailable.insert(output.unavailable.end(),result.unavailable.begin(),result.unavailable.end());
        output.body_fields=binding_origin_body_fields(expanded);
        snapshots->functions.insert_or_assign(caller.name,std::move(output));
    }catch(const Missing&) {
        snapshots->functions.erase(caller.name);unavailable(BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);
    }catch(const std::invalid_argument&) {
        snapshots->functions.erase(caller.name);unavailable(BindingSourceComponent::Body,BindingGap::InlineOriginUnavailable);
    }
    result.snapshots=std::move(snapshots);return result;
}
bool valid_scoped_binding_source(const Module& module,const Function& fn,const ScopedBindingSource& row) {
    if(!row.scope || !row.complete || !row.source.complete || !row.semantic || !module.summaries)return false;
    const auto actual=std::find_if(fn.inline_scopes.begin(),fn.inline_scopes.end(),[&](const auto& s){return s.identity==row.scope;});
    if(actual==fn.inline_scopes.end() || actual->parent!=row.parent || actual->callee!=row.source_function)return false;
    const auto semantic=module.summaries->semantic_snapshots().find(row.semantic_key);
    if(semantic==module.summaries->semantic_snapshots().end() ||
       canonical_encode_value(semantic->second).bytes!=canonical_encode_value(*row.semantic).bytes)return false;
    if(row.begin.block>=fn.blocks.size() || row.begin.index>=fn.blocks[row.begin.block].instructions.size())return false;
    const auto* begin=std::get_if<InlineScopeBegin>(&fn.blocks[row.begin.block].instructions[row.begin.index]);
    if(!begin || begin->scope!=row.scope || row.parameters.size()!=row.source.semantic_parameter_for_ir_parameter.size())return false;
    for(std::size_t i=0;i<row.parameters.size();++i) {
        const auto& p=row.parameters[i];
        if(p.ir_parameter!=i || p.semantic_parameter!=row.source.semantic_parameter_for_ir_parameter[i] ||
           (p.semantic_parameter && *p.semantic_parameter>=row.semantic->exposed_reads.size()) ||
           p.at.block!=row.begin.block || p.at.index<=row.begin.index || p.at.index>=fn.blocks[p.at.block].instructions.size())return false;
        const auto slot=std::find_if(actual->slots.begin(),actual->slots.end(),[&](const auto& s){return s.name==p.storage;});
        if(slot==actual->slots.end() || slot->type!=p.type)return false;
        const auto& ins=fn.blocks[p.at.block].instructions[p.at.index];
        if(p.writable) {
            const auto* bind=std::get_if<InlineBindAddress>(&ins);
            if(!bind || p.value || !p.address || bind->name!=p.storage || bind->address!=*p.address || bind->type!=p.type)return false;
        } else {
            const auto* bind=std::get_if<StoreLocal>(&ins);
            if(!bind || p.address || !p.value || bind->name!=p.storage || bind->value!=p.value || bind->type!=p.type || bind->borrowed!=p.borrowed)return false;
        }
    }
    if(row.returns_complete) {
        try {
            std::set<InstrRef,InlineSiteLess> originals,stores;
            for(const auto& returned:row.returns) {
                if(!originals.insert(returned.original).second || !stores.insert(returned.cleanup).second)return false;
                valid_return_binding(fn,row,returned);
            }
        }catch(const Missing&){return false;}
        catch(const std::invalid_argument&){return false;}
    }
    return true;
}
const InlineScope* inline_binding_scope(const Function& fn,BlockId block) {
    if(block>=fn.blocks.size())return nullptr;
    const InlineScope* best=nullptr;std::size_t depth=0;
    for(const auto& candidate:fn.inline_scopes) {
        if(std::find(candidate.blocks.begin(),candidate.blocks.end(),fn.blocks[block].label)==candidate.blocks.end())continue;
        std::size_t n=1;std::uint64_t parent=candidate.parent;std::set<std::uint64_t> seen{candidate.identity};
        while(parent) {
            if(!seen.insert(parent).second)return nullptr;
            const auto p=std::find_if(fn.inline_scopes.begin(),fn.inline_scopes.end(),[&](const auto& row){return row.identity==parent;});
            if(p==fn.inline_scopes.end())return nullptr;
            parent=p->parent;++n;
        }
        if(n==depth && best && best->identity!=candidate.identity)return nullptr;
        if(n>depth){best=&candidate;depth=n;}
    }
    return best;
}
const ScopedBindingSource* scoped_binding_source(const BindingSources& sources,const Function& fn,BlockId block) {
    const auto* best=inline_binding_scope(fn,block);if(!best)return nullptr;
    const auto owned=sources.scopes.find(fn.name);if(owned==sources.scopes.end())return nullptr;
    const auto found=owned->second.find(best->identity);
    return found!=owned->second.end() && found->second.complete?&found->second:nullptr;
}
std::vector<const FunctionBindingSource*> function_binding_sources(const BindingSources& sources,std::string_view function) {
    std::vector<const FunctionBindingSource*> result;
    if(const auto own=sources.functions.find(function);own!=sources.functions.end())result.push_back(&own->second);
    if(const auto scoped=sources.scopes.find(function);scoped!=sources.scopes.end())
        for(const auto& [id,row]:scoped->second){(void)id;if(row.complete)result.push_back(&row.source);}
    return result;
}
} // namespace quidra::ir::analysis
