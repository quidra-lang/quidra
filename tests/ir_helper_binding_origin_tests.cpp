#include "ir/helper_binding_origins.hpp"
#include "ir/clone_contracts.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "ir/analysis/summary_facts.hpp"
#include "optimizer/helper_specialization.hpp"
#include "quidra/compiler.hpp"
#include "quidra/lowering.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
using namespace quidra::optimizer;
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
const Function& function(const Module& module,std::string_view name) {
    const auto found=std::find_if(module.functions.begin(),module.functions.end(),[&](const auto& fn){return fn.name==name;});
    if(found==module.functions.end())throw std::runtime_error("fixture function missing");
    return *found;
}
std::string named(const Module& module,std::string_view name) {
    for(const auto& fn:module.functions)
        if(fn.declaration && fn.declaration->kind==DeclarationIdentity::Kind::Function && fn.declaration->source_name==name)return fn.name;
    throw std::runtime_error("fixture source declaration missing");
}
void continuity() {
    std::string program=R"(class Item(int value)
class Model(Item first, Item second)
bool inspect(tensor<real32> data, Model input)
    bool state = data.is_tracked()
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
    return state
void apply(Model input)
    tensor<real32> plain = tensor.zeros<real32>([2])
    print(inspect(plain, input))
    tensor<real32> fresh = tensor.zeros<real32>([2])
    tensor<real32> tracked = fresh.track()
    print(inspect(tracked, input))
    Item[] collected = reflect.collect<Item>(input)
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
Model model = Model(Item(1), Item(2))
apply(model)
)";
    // An unused authored function supplies module growth capacity without
    // adding callers, fake entry facts or extra specialization eligibility.
    program+="\nvoid reserve_capacity()\n    int32 capacity = 0\n";
    for(int i=0;i<80;++i)program+="    capacity = capacity + 1\n";
    program+="    return\n";
    auto module=lower(check(program));
    const auto source=named(module,"inspect"),caller=named(module,"apply");
    module.summaries=std::make_shared<const ModuleSummaries>(apply_ir_summary_facts(module,*module.summaries,
        infer_ir_summary_facts(module,*module.summaries)));
    const auto source_body=binding_origin_body_fields(function(module,source));
    const auto original=module.binding_origins->functions.at(source).source;
    const auto original_caller=module.binding_origins->functions.at(caller).source;
    require(original.availability.traversals && original.traversals.size()==1 &&
            original_caller.availability.collections && original_caller.collections.size()==1 &&
            original_caller.availability.traversals && original_caller.traversals.size()==2,
            "actual lowering did not produce the concrete original/caller origins");
    const auto caller_collect=std::find_if(original_caller.traversals.begin(),original_caller.traversals.end(),
        [](const auto& row){return row.kind==BindingTraversalOrigin::Kind::Collect;});
    const auto caller_each=std::find_if(original_caller.traversals.begin(),original_caller.traversals.end(),
        [](const auto& row){return row.kind==BindingTraversalOrigin::Kind::Each;});
    require(caller_collect!=original_caller.traversals.end() && caller_each!=original_caller.traversals.end() &&
            caller_collect->output==original_caller.collections.at(0).array && !caller_each->output &&
            caller_collect->at==original_caller.collections.at(0).at &&
            caller_collect->paths==original_caller.collections.at(0).paths && caller_each->paths==caller_collect->paths,
            "collect/each origins lost their distinct typed query kinds and output contracts");
    std::vector<std::pair<InstrRef,Call>> calls;
    const auto& caller_before=function(module,caller);
    for(BlockId b=0;b<caller_before.blocks.size();++b)
        for(std::uint32_t i=0;i<caller_before.blocks[b].instructions.size();++i)
            if(const auto* call=std::get_if<Call>(&caller_before.blocks[b].instructions[i]);call && call->callee==source)
                calls.emplace_back(InstrRef{b,i},*call);
    require(calls.size()==2,"fixture did not lower two real helper calls");
    AnalysisCache cache(module);
    const auto owner_before=module.binding_origins;
    SpecializationOptions exhausted;exhausted.maximum_operations=0;
    const auto refused=HelperSpecialization::run(module,cache,{},exhausted);
    require(!refused.changed() && !refused.origin_owner && module.binding_origins==owner_before &&
            binding_origin_body_fields(function(module,source))==source_body,
            "refused helper transaction changed bodies or published origins");
    auto report=HelperSpecialization::run(module,cache);
    const auto helper_clones=std::count_if(report.clones.begin(),report.clones.end(),
        [&](const auto& clone){return clone.source==source;});
    require(helper_clones==2 && report.rewritten_calls>=2 && report.folded_tracking==1 && report.origin_owner,
            "actual closed tracking entries did not produce folded/unchanged helper receipts");
    require(module.binding_origins==owner_before,"helper published clone origins before semantic clone import");
    std::vector<CloneReference> imports;
    for(const auto& origin:report.clones)imports.push_back({origin.clone,origin.source,
        origin.unchanged_body?CloneIrProofs::PreserveUnchangedBody:CloneIrProofs::Invalidate});
    for(auto& fn:module.functions)
        if(const auto entry=report.entries.find(fn.name);entry!=report.entries.end())fn.validated_entry=entry->second;
    auto imported=bind_clone_contracts(module,imports);
    bool preserved=false,invalidated=false;
    for(const auto& row:imported.bindings) {
        require(row.reference_available && row.semantic_available,"helper clone lost original owned semantic binding");
        const auto origin=std::find_if(report.clones.begin(),report.clones.end(),[&](const auto& value){return value.clone==row.function;});
        require(origin!=report.clones.end(),"import reported an unrelated clone");
        if(origin->unchanged_body) {preserved=preserved||origin->source==source;require(row.ir_proofs_preserved,"zero-fold clone did not preserve unchanged BODY proofs");}
        else {invalidated=invalidated||origin->source==source;require(!row.ir_proofs_preserved,"folded tracking clone inherited stale IR proofs");}
    }
    require(preserved && invalidated,"fixture failed to distinguish unchanged and folded helpers");
    module.summaries=std::move(imported.summaries);
    module.fact_sources=std::move(imported.fact_sources);
    module.binding_origins=std::move(report.origin_owner);
    const auto read=read_binding_origins(module);
    require(read.inputs.functions.contains(source) && read.inputs.functions.contains(caller),
            "helper retarget erased the retained original/caller source");
    require(binding_origin_body_fields(function(module,source))==source_body &&
            canonical_encode_value(read.inputs.functions.at(source)).bytes==canonical_encode_value(original).bytes,
            "shared original helper body or typed source was changed");
    const auto& retained=read.inputs.functions.at(caller);
    require(canonical_encode_value(retained.collections).bytes==canonical_encode_value(original_caller.collections).bytes &&
            canonical_encode_value(retained.traversals).bytes==canonical_encode_value(original_caller.traversals).bytes,
            "retarget changed actual caller sites or ordered typed paths");
    for(const auto& [at,before]:calls) {
        const auto& after=std::get<Call>(function(module,caller).blocks.at(at.block).instructions.at(at.index));
        auto expected=before;expected.callee=after.callee;
        require(after.callee!=before.callee && canonical_encode_value(after).bytes==canonical_encode_value(expected).bytes &&
                canonical_location_bytes(canonical_encode_value(after).locations)==canonical_location_bytes(canonical_encode_value(expected).locations),
                "retarget changed actual arguments/output/type/site attribution");
        require(read.inputs.functions.contains(after.callee),"helper clone source row is missing");
        const auto& copied=read.inputs.functions.at(after.callee);
        require(copied.declaration.source_name==original.declaration.source_name &&
                copied.semantic_parameter_for_ir_parameter==original.semantic_parameter_for_ir_parameter &&
                copied.traversals.at(0).at==original.traversals.at(0).at &&
                copied.traversals.at(0).root==original.traversals.at(0).root &&
                copied.traversals.at(0).paths==original.traversals.at(0).paths &&
                copied.local_storage_paths==original.local_storage_paths,
                "clone source crossed semantic/formal/local/traversal namespaces");
        const auto* summary=module.summaries->find(after.callee);
        const auto* original_summary=module.summaries->find(source);
        require(summary && summary->semantic==original_summary->semantic &&
                summary->semantic_key==original_summary->semantic_key,
                "clone source lost the immutable original effect owner");
    }
}
} // namespace
int main() {try {continuity();std::cout<<"transactional helper binding origins: ok\n";return 0;}
catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
