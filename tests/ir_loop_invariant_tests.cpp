#include "ir/analysis/loops.hpp"
#include "ir/analysis/loop_invariants.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace quidra;using namespace quidra::ir;using namespace quidra::ir::analysis;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main(){try{
    Function function;function.name="loops";
    const auto number=Type::simple(TypeKind::Int64);
    function.blocks={{"entry",{ConstantInt{1,"5",number},StoreLocal{"fixed",1,number},Jump{"outer"}}},
        {"outer",{ConstantBool{2,true},Branch{2,"inner","exit"}}},
        {"inner",{LoadLocal{3,"fixed",number},Unary{4,"-",3,number},ConstantBool{5,true},Branch{5,"latch","after"}}},
        {"latch",{Jump{"inner"}}},{"after",{Jump{"outer"}}},{"exit",{ReturnVoid{}}}};
    const auto cfg=build_cfg(function);const auto dom=dominators(cfg);const auto loops=build_loops(cfg,dom);
    require(loops.loops.size()==2 && loops.loops[0].header==1 && loops.loops[1].header==2 &&
        loops.loops[1].parent==0 && loops.loops[1].depth==2 && loops.loops[0].preheader==0,
        "natural loop latches, nesting and dedicated preheader are deterministic");
    const auto slots=classify_slots(function);const auto ssa=build_local_ssa(function,cfg,dom,slots);
    const auto uses=def_use(function);const auto aliases=alias_analysis(function,uses,ssa,slots);
    const auto live=liveness(function,cfg,uses,slots,aliases,{},&ssa);
    ModuleSummaries summaries;LoopInvariantInputs inputs{uses,ssa,slots,aliases,live,summaries};
    auto facts=loop_invariants(function,cfg,dom,loops,inputs);
    require(facts.loops.size()==2 && std::binary_search(facts.loops[0].values.begin(),facts.loops[0].values.end(),ValueId{4}),
        "pure scalar chain from unchanged local is invariant inside nested loops");
    require(facts.loops[0].locals.at("fixed").value_unchanged,"unchanged reaching local is a proved invariant");
    function.blocks[3].instructions.insert(function.blocks[3].instructions.begin(),Call{0,"unknown",{},Type::simple(TypeKind::Void)});
    const auto changed_cfg=build_cfg(function);const auto changed_dom=dominators(changed_cfg);const auto changed_loops=build_loops(changed_cfg,changed_dom);
    const auto changed_uses=def_use(function);const auto changed_ssa=build_local_ssa(function,changed_cfg,changed_dom,slots);
    const auto changed_aliases=alias_analysis(function,changed_uses,changed_ssa,slots);const auto changed_live=liveness(function,changed_cfg,changed_uses,slots,changed_aliases,{},&changed_ssa);
    LoopInvariantInputs unknown{changed_uses,changed_ssa,slots,changed_aliases,changed_live,summaries};
    auto conservative=loop_invariants(function,changed_cfg,changed_dom,changed_loops,unknown);
    require(!conservative.loops[0].state_complete && !conservative.loops[0].locals.at("fixed").value_unchanged,
        "unknown callee cannot become an invariant or a no-write state proof");
    Function irreducible;irreducible.blocks={{"entry",{ConstantBool{1,true},Branch{1,"a","b"}}},
        {"a",{Jump{"b"}}},{"b",{Branch{1,"a","exit"}}},{"exit",{ReturnVoid{}}}};
    const auto bad_cfg=build_cfg(irreducible);const auto bad_loops=build_loops(bad_cfg,dominators(bad_cfg));
    require(bad_loops.irreducible.size()==1 && bad_loops.irreducible[0].entries==std::vector<BlockId>{1,2} &&
        !bad_loops.loop_available[1] && !bad_loops.loop_available[2],"multiple-entry cyclic SCC is explicitly unavailable");
    Function tensors;tensors.name="state";const auto tensor=Type::tensor(Type::simple(TypeKind::Real32),1);
    tensors.blocks={{"entry",{ConstantInt{1,"2",number},ArrayMake{2,{1},Type::array(number,1)},
        TensorCreate{3,2,{},tensor,tensor_fill_mode::zeros},StoreLocal{"state",3,tensor},Jump{"header"}}},
        {"header",{LoadLocal{4,"state",tensor},TensorShape{5,4,Type::array(number)},ConstantBool{6,true},Branch{6,"body","exit"}}},
        {"body",{Jump{"header"}}},{"exit",{ReturnVoid{}}}};
    auto analyze_state=[&](const Function& current) {
        const auto graph=build_cfg(current);const auto dominance=dominators(graph);const auto regions=build_loops(graph,dominance);
        const auto local_slots=classify_slots(current);const auto local_ssa=build_local_ssa(current,graph,dominance,local_slots);
        const auto def_uses=def_use(current);const auto storage=alias_analysis(current,def_uses,local_ssa,local_slots);
        const auto lifetime=liveness(current,graph,def_uses,local_slots,storage,{},&local_ssa);
        LoopInvariantInputs state_inputs{def_uses,local_ssa,local_slots,storage,lifetime,summaries};
        state_inputs.regions={{17,{2,0},{{2,0}}}};
        return loop_invariants(current,graph,dominance,regions,state_inputs);
    };
    auto unchanged=analyze_state(tensors);
    require(unchanged.loops[0].locals.at("state").metadata.shape && unchanged.loops[0].locals.at("state").metadata.device &&
        std::binary_search(unchanged.loops[0].values.begin(),unchanged.loops[0].values.end(),ValueId{5}),
        "unexposed tensor descriptor queries and local shape/device are invariant without writes");
    tensors.blocks[2].instructions.insert(tensors.blocks[2].instructions.begin(),TensorClearGrad{4});
    auto mutated=analyze_state(tensors);
    require(!mutated.loops[0].locals.at("state").metadata.tracked &&
        !std::binary_search(mutated.loops[0].values.begin(),mutated.loops[0].values.end(),ValueId{5}),
        "descriptor mutation cannot become an invariant query or local descriptor fact");
    require(!mutated.loops[0].storage.empty() && mutated.loops[0].storage[0].region_only_writes==17 &&
        !mutated.loops[0].storage[0].exclusive_ownership_available && mutated.loops[0].storage[0].backedge_live_available &&
        mutated.loops[0].storage[0].outside_region_live,
        "region membership records writes and outside backedge-live aliases without inventing physical exclusivity");
    std::puts("IR loops: nesting, pure/local invariance and opaque/irreducible rejection verified");
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
