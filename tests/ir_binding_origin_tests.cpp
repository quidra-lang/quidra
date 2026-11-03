#include "ir/binding_origins.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "quidra/compiler.hpp"
#include "quidra/lowering.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
const Function& function(const Module& module,std::string_view source) {
    for(const auto& row:module.functions)
        if(row.declaration && row.declaration->kind==DeclarationIdentity::Kind::Function &&
           row.declaration->source_name==source)return row;
    throw std::runtime_error("source declaration is missing");
}
const BindingOriginSnapshot& snapshot(const Module& module,const Function& fn) {
    return module.binding_origins->functions.at(fn.name);
}
bool stale(const BindingOriginRead& read,std::string_view function) {
    for(const auto& issue:read.unavailable)
        if(issue.lookup==function && issue.reason==BindingGap::StaleOrigin)return true;
    return false;
}
void actual_producer() {
    auto checked=check(R"(class Item(int value)
class Model(Item first, Item second)
class Holder this(local Model &input)
    private Model &model
    &model = &input

Item[] collect(Model input)
    return reflect.collect<Item>(input)
void visit(Model input)
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
string[] paths(Model input)
    return reflect.each_paths<Item>(input)
void mutate(Model &input)
    for Item &item = &reflect.each<Item>(&input)[]
        item.value = 7
int plain(int input)
    int copied = input
    return copied
)");
    auto module=lower(checked);
    require(module.binding_origins!=nullptr,"lowering omitted owned binding origins");
    const auto& collect=function(module,"collect");
    const auto& visit=function(module,"visit");
    const auto& paths=function(module,"paths");
    const auto& mutate=function(module,"mutate");
    const auto& plain=function(module,"plain");
    const std::vector<std::vector<semantics::PathStep>> order{
        {semantics::PathStep::field_named("first")},
        {semantics::PathStep::field_named("second")}};
    const auto& collection=snapshot(module,collect).source;
    require(collection.availability.collections && collection.collections.size()==1 &&
            collection.collections[0].complete && collection.collections[0].paths==order,
            "reflect.collect lost actual ordered field origins");
    const auto& each=snapshot(module,visit).source;
    require(each.availability.traversals && each.traversals.size()==1 &&
            each.traversals[0].kind==BindingTraversalOrigin::Kind::Each &&
            each.traversals[0].complete && !each.traversals[0].output &&
            !each.traversals[0].writable && each.traversals[0].paths==order,
            "reflect.each fabricated an output or lost the fixed traversal");
    const auto& path_query=snapshot(module,paths).source;
    require(path_query.traversals.size()==1 &&
            path_query.traversals[0].kind==BindingTraversalOrigin::Kind::EachPaths &&
            path_query.traversals[0].complete && path_query.traversals[0].output &&
            path_query.traversals[0].target==each.traversals[0].target &&
            path_query.traversals[0].paths==order,
            "reflect.each_paths lost typed target, real output or path order");
    const auto& mutable_query=snapshot(module,mutate).source;
    require(mutable_query.traversals.size()==1 && mutable_query.traversals[0].writable &&
            mutable_query.traversals[0].root_address &&
            !mutable_query.validated_initialization.contains(0),
            "reference requirements became a validated value-entry proof");
    const auto& entry=collection.validated_initialization.at(0);
    require(collection.availability.validated_initialization &&
            std::find(entry.begin(),entry.end(),std::vector<semantics::PathStep>{
                semantics::PathStep::field_named("first"),semantics::PathStep::field_named("value")})!=entry.end(),
            "actual nonwritable checker entry proof was not exported");
    const auto& empty=snapshot(module,plain).source;
    require(empty.availability.collections && empty.collections.empty() &&
            empty.availability.traversals && empty.traversals.empty() &&
            empty.availability.rebind_events && empty.rebind_events.empty() &&
            empty.availability.local_storage_paths && !empty.local_storage_paths.empty(),
            "captured empty components or exact renamed locals became unknown");
    bool binding=false;
    for(const auto& fn:module.functions) {
        if(!fn.declaration || fn.declaration->kind!=DeclarationIdentity::Kind::Constructor)continue;
        const auto& source=snapshot(module,fn).source;
        for(const auto& event:source.rebind_events) {
            require(event.kind==BindingRebindEvent::Kind::Field &&
                    source.availability.rebind_events && !source.rebind && !source.availability.rebind_role,
                    "actual binding event was replaced by a method role guess");
            const auto* bind=std::get_if<BindFieldReference>(&fn.blocks.at(event.at.block).instructions.at(event.at.index));
            require(bind && bind->object==event.object && bind->index==event.field && bind->address==event.address,
                    "reference-storage rewriting lost the exact binding instruction");
            binding=true;
        }
    }
    require(binding,"constructor reference binding event was not captured");
    const auto valid=read_binding_origins(module);
    require(valid.inputs.functions.contains(visit.name),"fresh concrete producer row is unavailable");
    const auto name=visit.name;
    auto& body=*std::find_if(module.functions.begin(),module.functions.end(),[&](const auto& row){return row.name==name;});
    const auto before=binding_origin_body_fields(body);
    body.next_value+=100;
    body.tensor_regions.emplace_back();
    require(before==binding_origin_body_fields(body) && read_binding_origins(module).inputs.functions.contains(name),
            "nonbody analysis annotations erased the source binding snapshot");
    auto clone=body;clone.name="explicit.clone";
    std::vector<BindingSourceIssue> issues;
    require(clone_binding_origin(body,clone,snapshot(module,body),issues).has_value(),
            "explicit unchanged-body clone lost exact origins");
    AnalysisCache cache(module);
    const auto& cached=cache.binding_sources().functions.at(name);
    require(cached.availability.traversals && cached.traversals.size()==1,
            "cache did not adapt the valid concrete traversal source");
    body.blocks.front().instructions.insert(body.blocks.front().instructions.begin(),StatementBoundary{999});
    const auto changed=read_binding_origins(module);
    require(!changed.inputs.functions.contains(name) && stale(changed,name),
            "instruction rewrite silently reused stale sites or locals");
    cache.invalidate(static_cast<std::size_t>(&body-module.functions.data()));
    const auto& refreshed=cache.binding_sources();
    const auto& source=refreshed.functions.at(name);
    require(!source.availability.traversals && source.traversals.empty() &&
            std::any_of(refreshed.issues.begin(),refreshed.issues.end(),[&](const auto& issue){
                return issue.lookup==name && issue.reason==BindingGap::StaleOrigin;
            }),"cache invalidation reused a stale concrete traversal origin");
}
} // namespace
int main() {
    try {actual_producer();std::cout<<"owned binding-origin producer: ok\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
