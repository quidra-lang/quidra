#include "ir/facts/tensor.hpp"
#include "ir/analysis/tensor_facts.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir::facts;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
TensorFactAnalysis analyze(const Function& function,const ModuleSummaries& summaries,const FactSources& sources,const ParamFacts& params={}) {
    const auto cfg=build_cfg(function);const auto dom=dominators(cfg);const auto slots=classify_slots(function);
    const auto ssa=build_local_ssa(function,cfg,dom,slots);
    return compute_tensor_facts(function,cfg,dom,ssa,slots,summaries,sources,params);
}
}
int main() {
    try {
        const auto element = Type::simple(TypeKind::Real32);
        const auto n = ShapeExtent::symbol(31), m = ShapeExtent::symbol(44);
        auto left = Type::tensor(element, 2, {}, {n, 1});
        auto right = Type::tensor(element, 2, {}, {1, m});
        TensorShapeTransfer broadcast;
        broadcast.operation = TensorShapeOp::Broadcast;
        broadcast.result = Type::tensor(element, 2);
        broadcast.inputs = {left, right};
        auto shape = transfer_tensor_shape(broadcast);
        require(same_extent(tensor_extent(shape, 0), n) && same_extent(tensor_extent(shape, 1), m),
            "shared broadcast transfer retains canonical symbolic identities");
        TensorShapeTransfer transpose;
        transpose.operation = TensorShapeOp::Transpose;
        transpose.result = Type::tensor(element, 2);
        transpose.inputs = {shape};
        transpose.axes = std::pair<std::int64_t, std::int64_t>{0, 1};
        shape = transfer_tensor_shape(transpose);
        require(same_extent(tensor_extent(shape, 0), m) && same_extent(tensor_extent(shape, 1), n),
            "shared transpose transfer uses actual axes");
        const auto ellipsis = Type::tensor(element, 4, {2, 7}, {}, 1);
        require(tensor_extent(ellipsis, 0).known() == 2 && tensor_extent(ellipsis, 3).known() == 7 &&
            tensor_extent(ellipsis, 1).kind == ShapeExtent::Kind::Unknown,
            "head-tail ellipsis aligns with physical rank and preserves the omitted middle");
        constexpr auto sparse=std::numeric_limits<ValueId>::max();
        Function guarded;guarded.name="guarded";
        const auto declared=Type::tensor(element,-1,{2,7},{},1);
        guarded.parameters={{"value",declared}};
        ShapedConstraintCheck check;check.value=sparse;check.extents={1,2};check.ellipsis=1;
        guarded.blocks={{"entry",{
            LoadLocal{sparse,"value",declared},ConstantInt{1,"2",Type::simple(TypeKind::Int64)},ConstantInt{2,"7",Type::simple(TypeKind::Int64)},check,
            TensorShape{3,sparse,Type::array(Type::simple(TypeKind::Int64))},ArrayLength{4,3},ConstantInt{5,"4",Type::simple(TypeKind::Int64)},ExtentEqualCheck{4,5},
            LoadLocal{sparse-1,"value",declared},ConstantBool{6,true},Branch{6,"left","right"}}},
            {"left",{Call{0,"reader",{},Type::simple(TypeKind::Void)},Jump{"join"}}},
            {"right",{Call{0,"writer",{},Type::simple(TypeKind::Void)},Jump{"join"}}},
            {"join",{ReturnVoid{}}},{"dead",{LoadLocal{7,"value",declared},ReturnVoid{}}}};
        Module module;module.functions={guarded};
        Function reader;reader.name="reader";reader.blocks={{"entry",{ReturnVoid{}}}};
        Function writer=reader;writer.name="writer";
        module.functions.push_back(reader);module.functions.push_back(writer);
        Function scalar_source;scalar_source.name="scalar_source";scalar_source.result=Type::simple(TypeKind::Int64);scalar_source.external_symbol="native_scalar";
        module.functions.push_back(scalar_source);
        semantics::EffectSummaries semantics;
        semantics::EffectSummary function_effect;function_effect.exposed_reads.resize(1);function_effect.exposed_writes.resize(1);
        semantics.set("checked.guarded",function_effect);semantics.set("checked.reader",{});
        semantics.set("checked.scalar_source",{});
        semantics::EffectSummary writer_effect;writer_effect.mutates_ambient=true;semantics.set("checked.writer",writer_effect);
        SummaryBindings bindings{{"guarded",{"checked.guarded",{0}}},{"reader",{"checked.reader",{}}},{"writer",{"checked.writer",{}}},{"scalar_source",{"checked.scalar_source",{}}}};
        auto summaries=adapt_summaries(module,semantics,bindings);
        FactSources sources;sources.policies.writes_policy={{"reader",false},{"writer",true}};
        ParamFacts parameters;parameters.policies={{"policy","fast"}};
        auto facts=analyze(guarded,summaries,sources,parameters);
        require(facts.at({0,3},sparse).type.length<0 && facts.at({0,3},sparse).head_extents.empty(),
            "declared signature facts cannot prove the entry check that establishes them");
        const auto after_pattern=facts.at({0,4},sparse);
        require(after_pattern.type.length<0 && after_pattern.minimum_rank==2 && after_pattern.head_extents[0].known()==2 && after_pattern.tail_extents[0].known()==7,
            "validated unknown-rank head-tail pattern is available only after its guard");
        require(!same_extent(facts.scalar_at({0,7},4),facts.scalar_at({0,7},5)),"rank equality cannot prove itself before its guard");
        const auto after_rank=facts.at({0,8},sparse);
        require(after_rank.type.length==4 && after_rank.extents[0].known()==2 && after_rank.extents[3].known()==7,
            "later rank guard aligns validated tail axes with actual physical rank");
        require(facts.at({0,10},sparse-1).type.length==4,"later loads inherit validation through the exact reaching parameter");
        require(facts.policy_at({1,1})->at("policy")=="fast" && facts.policy_at({2,1})->empty() && facts.policy_at({3,0})->empty(),
            "known reader preserves policy, writer kills it and diamond join is a must fact");
        require(!facts.at({4,1},7).has_type,"unreachable facts remain unavailable");
        Function loop;loop.name="loop";const auto tensor=Type::tensor(element);
        loop.blocks={{"entry",{ConstantInt{1,"2",Type::simple(TypeKind::Int64)},ArrayMake{2,{1},Type::array(Type::simple(TypeKind::Int64),1)},
            TensorCreate{3,2,{},Type::tensor(element,1),tensor_fill_mode::zeros},StoreLocal{"x",3,tensor},Jump{"header"}}},
            {"header",{LoadLocal{4,"x",tensor},ConstantBool{5,true},Branch{5,"body","exit"}}},
            {"body",{Unary{6,"-",4,tensor},StoreLocal{"x",6,tensor},Jump{"header"}}},
            {"exit",{LoadLocal{8,"x",tensor},Return{8,tensor}}}};
        auto invariant=analyze(loop,summaries,sources);
        require(invariant.at({1,2},4).type.length==1 && invariant.at({1,2},4).extents[0].known()==2 &&
            invariant.at({3,1},8).device.kind==BackendKind::Cpu,"SSA loop reaches the actual shape/device fixed point");
        loop.blocks[2].instructions={Call{10,"scalar_source",{},Type::simple(TypeKind::Int64)},ArrayMake{11,{10},Type::array(Type::simple(TypeKind::Int64),1)},
            TensorCreate{12,11,{},Type::tensor(element,1),tensor_fill_mode::zeros},StoreLocal{"x",12,tensor},Jump{"header"}};
        auto varying=analyze(loop,summaries,sources);
        require(!same_extent(varying.at({1,2},4).extents[0],varying.at({2,4},12).extents[0]),
            "body-created extent symbols never pass the loop header as invariants");
        loop.parameters={{"input",Type::tensor(element,1)}};
        loop.blocks[0].instructions[2]=LoadLocal{3,"input",Type::tensor(element,1)};
        loop.blocks[2].instructions={Unary{6,"-",4,tensor},StoreLocal{"x",6,tensor},Jump{"header"}};
        TensorFacts validated;validated.has_type=true;validated.type=Type::tensor(element,1);validated.extents={ShapeExtent::symbol(37)};
        validated.minimum_rank=1;validated.rank_extent=ShapeExtent(1);
        ParamFacts proven;proven.tensors.emplace(0,validated);
        auto external_symbol=analyze(loop,summaries,sources,proven);
        require(same_extent(external_symbol.at({1,2},4).extents[0],ShapeExtent::symbol(37)),
            "validated caller symbols are external loop origins and retain identity");
        Function fallible;fallible.name="fallible";const auto converted=Type::tensor(Type::simple(TypeKind::Int32),1);
        fallible.blocks={{"entry",{ConstantInt{1,"2",Type::simple(TypeKind::Int64)},ArrayMake{2,{1},Type::array(Type::simple(TypeKind::Int64),1)},
            TensorCreate{3,2,{},Type::tensor(element,1),tensor_fill_mode::zeros},
            TensorCast{13,3,Type::tensor(element,1),converted,Type::union_of({converted,Type::simple(TypeKind::Error)})},ReturnVoid{}}}};
        auto carrier=analyze(fallible,summaries,sources);
        require(!carrier.at({0,4},13).has_type,"fallible cast union carrier is never mistaken for its success tensor");
        Function stale;stale.name="stale";
        stale.blocks={{"entry",{ConstantInt{1,"2",Type::simple(TypeKind::Int64)},ArrayMake{2,{1},Type::array(Type::simple(TypeKind::Int64),1)},
            TensorCreate{3,2,{},Type::tensor(element,1),tensor_fill_mode::zeros},StoreLocal{"x",3,tensor},
            TensorShape{4,3,Type::array(Type::simple(TypeKind::Int64))},ArrayLength{5,4},
            Call{0,"opaque",{{3,{}}},Type::simple(TypeKind::Void)},ExtentEqualCheck{5,1},LoadLocal{6,"x",tensor},Clone{7,6,tensor},
            TensorCreate{8,2,{},Type::tensor(element,1),tensor_fill_mode::zeros},ReturnVoid{}}}};
        auto killed=analyze(stale,summaries,sources);
        require(killed.at({0,6},3).tracked==Tri::False,"opaque call arguments retain their before-instruction facts");
        const auto stale_value=killed.at({0,7},3);
        require(stale_value.tracked==Tri::Unknown && stale_value.device.kind==BackendKind::Unknown && stale_value.contiguous==Tri::Unknown &&
            stale_value.type.length<0,"opaque call must invalidate prior descriptor facts");
        require(killed.at({0,8},3).type.length<0,"old shape snapshot cannot validate a descriptor changed by an opaque call");
        require(killed.at({0,10},6).tracked==Tri::Unknown && killed.at({0,10},7).tracked==Tri::Unknown,
            "later loads and clones cannot resurrect killed tracking facts");
        require(killed.at({0,11},8).tracked==Tri::False && killed.at({0,11},8).type.length==1,
            "a genuinely fresh later definition establishes its own facts");
        Function diamond=stale;diamond.name="one_path";
        diamond.blocks[0].instructions.resize(4);diamond.blocks[0].instructions.push_back(ConstantBool{9,true});
        diamond.blocks[0].instructions.push_back(Branch{9,"left","right"});
        diamond.blocks.push_back({"left",{Call{0,"opaque",{{3,{}}},Type::simple(TypeKind::Void)},Jump{"join"}}});
        diamond.blocks.push_back({"right",{Jump{"join"}}});
        diamond.blocks.push_back({"join",{LoadLocal{6,"x",tensor},ReturnVoid{}}});
        auto joined=analyze(diamond,summaries,sources);
        require(joined.at({3,1},3).tracked==Tri::Unknown && joined.at({3,1},6).tracked==Tri::Unknown,
            "one-sided mutation remains unavailable at a CFG join");

        Function scoped;scoped.name="scoped";InlineScope frame;frame.identity=8;frame.blocks={"entry"};
        frame.slots={{"object",Type::class_type("arbitrary_drop"),InlineSlotKind::OwnedLocal}};scoped.inline_scopes={frame};
        scoped.blocks={{"entry",{ConstantInt{1,"2",Type::simple(TypeKind::Int64)},ArrayMake{2,{1},Type::array(Type::simple(TypeKind::Int64),1)},
            TensorCreate{3,2,{},Type::tensor(element,1),tensor_fill_mode::zeros},InlineScopeBegin{8},InlineScopeCleanup{8},ReturnVoid{}}}};
        ParamFacts policy;policy.policies={{"policy","fast"}};
        auto unsafe_cleanup=analyze(scoped,summaries,sources,policy);
        require(unsafe_cleanup.at({0,5},3).tracked==Tri::Unknown && unsafe_cleanup.policy_at({0,5})->empty(),
            "unknown user-drop cleanup kills descriptor and policy facts");
        scoped.inline_scopes[0].slots[0].type=Type::simple(TypeKind::Int64);
        auto safe_cleanup=analyze(scoped,summaries,sources,policy);
        require(safe_cleanup.at({0,5},3).tracked==Tri::False && !safe_cleanup.policy_at({0,5})->empty(),
            "known primitive cleanup preserves unrelated descriptor and policy facts");
        semantics::EffectSummary callback_semantic;ModuleSummaries opaque_cleanup;
        FunctionSummary callback;callback.semantic=&callback_semantic;callback.effects=effect_bit::Opaque;callback.effects_complete=false;
        opaque_cleanup.by_function.emplace("cleanup_callback",callback);
        scoped.blocks[0].instructions[4]=Call{0,"cleanup_callback",{},Type::simple(TypeKind::Void)};
        auto through_call=analyze(scoped,opaque_cleanup,sources,policy);
        require(through_call.policy_at({0,5})->empty(),"an opaque cleanup summary cannot resurrect the old semantic no-policy-write assumption");
        scoped.blocks[0].instructions[4]=ParallelBlock{};
        require(analyze(scoped,summaries,sources,policy).at({0,5},3).tracked==Tri::Unknown,
            "parallel boundary never retains unsupported descriptor mutation proofs");
        std::puts("IR tensor facts: shared transfers, scoped guards, sparse SSA loops and policy joins verified");
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
