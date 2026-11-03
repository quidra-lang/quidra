#include "ir/check_binding_origins.hpp"
#include "optimizer/check_elimination.hpp"
#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "quidra/compiler.hpp"
#include "quidra/lowering.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
using namespace quidra::optimizer;
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
CheckOriginAction action(CheckAction value) {
    switch(value) {
    case CheckAction::Erase:return CheckOriginAction::Erase;
    case CheckAction::BoundsFlag:return CheckOriginAction::BoundsFlag;
    case CheckAction::InitializationFlag:return CheckOriginAction::InitializationFlag;
    case CheckAction::OverflowFlag:return CheckOriginAction::OverflowFlag;
    case CheckAction::InlineFlag:return CheckOriginAction::InlineFlag;
    case CheckAction::BroadcastFlag:return CheckOriginAction::BroadcastFlag;
    case CheckAction::ShapeFlag:return CheckOriginAction::ShapeFlag;
    }
    throw std::runtime_error("unknown check action");
}
CheckOriginTransfer receipt(const Function& before,const CheckEliminationReport& report) {
    CheckOriginTransfer result;result.function=before.name;
    std::set<InstrRef,InlineSiteLess> erased;
    for(const auto& proof:report.certificates) {
        result.edits.push_back({proof.at,action(proof.action)});
        if(proof.action==CheckAction::Erase)erased.insert(proof.at);
    }
    for(BlockId b=0;b<before.blocks.size();++b) {
        std::uint32_t next=0;
        for(std::uint32_t i=0;i<=before.blocks[b].instructions.size();++i) {
            result.boundaries.emplace(InstrRef{b,i},InstrRef{b,next});
            if(i==before.blocks[b].instructions.size())break;
            if(erased.contains({b,i}))result.instructions.emplace(InstrRef{b,i},std::nullopt);
            else result.instructions.emplace(InstrRef{b,i},InstrRef{b,next++});
        }
    }
    return result;
}
void actual_scoped_sources() {
    std::string program=R"(class Item(int32 value)
class Model(Item first, Item second)
int32 examine(Model input)
    int32 stride = 1
    int32 result = 1 + 2
    for 0 <= int counter < 2 step int(stride)
        print(counter)
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
    return result
int32 drive(Model input)
    int32 padding = 0
)";
    for(std::size_t i=0;i<40;++i)program+="    padding = padding + 1\n";
    program+=R"(    int32 result = examine(input)
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
    return result
)";
    auto module=lower(check(program));
    const auto drive=std::find_if(module.functions.begin(),module.functions.end(),[](const auto& fn){
        return fn.declaration && fn.declaration->kind==DeclarationIdentity::Kind::Function && fn.declaration->source_name=="drive";
    })->name;
    Function reserve;reserve.name="fixture.reserve";
    reserve.blocks.push_back({"entry",{}});
    for(ValueId id=1;id<=500;++id)reserve.blocks.front().instructions.push_back(ConstantBool{id,true});
    reserve.blocks.front().instructions.push_back(ReturnVoid{});reserve.next_value=501;
    module.functions.push_back(std::move(reserve));
    const auto registry=RuleRegistry::build(module);
    InliningOptions options;options.ordinary_instruction_limit=400;
    require(BudgetedInliner::run(module,registry,options).changed(),"actual source helper was not inlined");
    const auto index=static_cast<std::size_t>(std::find_if(module.functions.begin(),module.functions.end(),[&](const auto& fn){return fn.name==drive;})-module.functions.begin());
    const auto before=module.functions.at(index);
    const auto origins=module.binding_origins->functions.at(drive);
    require(origins.root_source_available && !origins.source.traversals.empty() && !origins.scopes.empty(),
            "actual lowering/inlining did not produce root and scoped sources");
    const auto plan=CheckElimination::analyze(module,index);
    if(!plan.ready() || !plan.report().removed || !plan.report().flagged)
        throw std::runtime_error("actual source did not produce both a proven check erase and flag: ready="+
            std::to_string(plan.ready())+", removed="+std::to_string(plan.report().removed)+
            ", flagged="+std::to_string(plan.report().flagged)+", failure="+
            std::to_string(static_cast<unsigned>(plan.report().failure)));
    bool scoped_step=false;
    for(const auto& proof:plan.report().certificates) {
        if(proof.kind!=CheckKind::ForStep || proof.action!=CheckAction::Erase ||
           proof.rule!=ProofRule::LiteralStepSign)continue;
        const auto* scope=inline_binding_scope(before,proof.at.block);
        require(scope && origins.scopes.contains(scope->identity) &&
                std::holds_alternative<ForStepCheck>(before.blocks[proof.at.block].instructions[proof.at.index]),
                "step erase did not belong to an actual copied helper check");
        scoped_step=true;
    }
    require(scoped_step,"actual copied step check had no nonzero/sign certificate");
    std::cout<<"source certificates: erases="<<plan.report().removed<<", flags="<<plan.report().flagged<<'\n';
    auto staged=module;
    require(CheckElimination::apply(staged,plan).applied,"actual check plan refused its own preimage");
    const auto after=staged.functions.at(index);
    auto edits=receipt(before,plan.report());
    std::uint64_t operations=0;
    std::size_t encoded_bytes=0;
    const auto prepared=transfer_check_origins(module,before,after,edits,[&]{++operations;},64*1024*1024,
        [&](std::size_t n){encoded_bytes+=n;});
    require(prepared.accepted && prepared.snapshots && operations && encoded_bytes,"typed check origin transfer was refused");
    staged.binding_origins=prepared.snapshots;
    const auto read=read_binding_origins(staged);
    require(read.inputs.functions.contains(drive) && read.inputs.scopes.at(drive).size()==origins.scopes.size(),
            "accepted check edits lost root or scoped source ownership");
    const auto& root=read.inputs.functions.at(drive);
    require(root.declaration==origins.source.declaration && root.local_storage_paths==origins.source.local_storage_paths &&
            root.traversals.at(0).paths==origins.source.traversals.at(0).paths &&
            root.traversals.at(0).at==edits.boundaries.at(origins.source.traversals.at(0).at),
            "check erase flattened or failed to relocate retained root traversal");
    for(const auto& [id,old]:origins.scopes) {
        const auto& row=read.inputs.scopes.at(drive).at(id);
        require(valid_scoped_binding_source(staged,after,row) && row.source.declaration==old.source.declaration &&
                row.source.local_storage_paths==old.source.local_storage_paths && row.parameters.size()==old.parameters.size() &&
                row.returns.size()==old.returns.size(),"check edit changed the owned callee namespace");
        for(std::size_t i=0;i<row.returns.size();++i)
            require(row.returns[i].original==old.returns[i].original && row.returns[i].original_value==old.returns[i].original_value &&
                    row.returns[i].cleanup==*edits.instructions.at(old.returns[i].cleanup),
                    "check edit altered original return coordinates or missed actual cleanup remap");
    }
    const auto rejected=[&](const Function& changed,const CheckOriginTransfer& claimed) {
        const auto result=transfer_check_origins(module,before,changed,claimed,[]{});
        require(!result.accepted && result.snapshots==module.binding_origins,
                "invalid check receipt published a changed source owner");
    };
    auto invalid=after;invalid.result=Type::simple(TypeKind::String);rejected(invalid,edits);
    invalid=after;invalid.source_file="different.qui";rejected(invalid,edits);
    invalid=after;invalid.blocks.front().instructions.push_back(ReturnVoid{});rejected(invalid,edits);
    auto bad=edits;bad.boundaries.begin()->second.index++;rejected(after,bad);
    bad=edits;bad.edits.push_back({{0,0},CheckOriginAction::InlineFlag});rejected(after,bad);
    const auto& first_scope=origins.scopes.begin()->second;
    bad=edits;bad.instructions.at(first_scope.begin)=std::nullopt;rejected(after,bad);
    bool removed_observable=false;
    for(const auto& [old,target]:edits.instructions) {
        if(!target)continue;
        const auto& instruction=before.blocks[old.block].instructions[old.index];
        if(!std::holds_alternative<Call>(instruction) && !std::holds_alternative<Print>(instruction))continue;
        invalid=after;bad=edits;
        invalid.blocks[target->block].instructions.erase(invalid.blocks[target->block].instructions.begin()+target->index);
        bad.edits.push_back({old,CheckOriginAction::Erase});bad.instructions.at(old).reset();
        for(auto& [site,mapped]:bad.instructions) {
            (void)site;
            if(mapped && mapped->block==target->block && mapped->index>target->index)--mapped->index;
        }
        for(auto& [site,mapped]:bad.boundaries) {
            (void)site;
            if(mapped.block==target->block && mapped.index>target->index)--mapped.index;
        }
        rejected(invalid,bad);removed_observable=true;break;
    }
    require(removed_observable,"generic source had no observable instruction for the forbidden erase regression");
    invalid=after;
    const auto cleanup=*edits.instructions.at(first_scope.returns.front().cleanup);
    std::get<InlineScopeCleanup>(invalid.blocks[cleanup.block].instructions[cleanup.index]).scope++;
    rejected(invalid,edits);
    auto annotated=module;
    auto annotated_before=before;annotated_before.tensor_regions.emplace_back();
    annotated.functions.at(index)=annotated_before;
    auto annotated_after=after;annotated_after.tensor_regions=annotated_before.tensor_regions;
    require(!transfer_check_origins(annotated,annotated_before,annotated_after,edits,[]{}).accepted,
            "shifted sites silently retained stale derived regions");
    edits.invalidate_tensor_regions=true;annotated_after.tensor_regions.clear();
    require(transfer_check_origins(annotated,annotated_before,annotated_after,edits,[]{}).accepted,
            "explicit derived region invalidation was refused");
    require(transfer_check_origins(module,before,after,receipt(before,plan.report()),[]{},1).failure==CheckOriginFailure::Budget,
            "bounded preimage exhaustion was not explicit");
    auto missing=module;missing.binding_origins.reset();
    const auto no_source=transfer_check_origins(missing,before,after,receipt(before,plan.report()),[]{});
    require(no_source.accepted && !no_source.snapshots && !no_source.unavailable.empty(),
            "source absence fabricated an origin or blocked a structurally valid check edit");
}
void composite_flags() {
    const auto integer=Type::simple(TypeKind::Int32);
    Module module;Function before;before.name="array.flags";before.result=integer;
    before.blocks.push_back({"entry",{
        ConstantBool{1,true},ConstantInt{2,"0",Type::simple(TypeKind::Nat)},ConstantInt{3,"7",integer},
        ArrayMake{4,{3},Type::array(integer)},
        ArrayGet{5,4,2,integer,{9,4},false,false,1,1},Return{5,integer}}});
    before.next_value=6;module.functions.push_back(before);
    const auto plan=CheckElimination::analyze(module,0);
    require(plan.ready() && plan.report().flagged==2,"true array guards did not produce both flag certificates");
    auto staged=module;require(CheckElimination::apply(staged,plan).applied,"composite flag plan refused");
    const auto edits=receipt(before,plan.report());
    require(transfer_check_origins(module,before,staged.functions.front(),edits,[]{}).accepted,
            "two certified flags on one actual instruction were not composed");
    auto invalid=staged.functions.front();std::get<ArrayGet>(invalid.blocks.front().instructions[4]).position.line++;
    require(!transfer_check_origins(module,before,invalid,edits,[]{}).accepted,
            "flag replacement hid a changed authored source field");
    struct Exhausted {};
    bool propagated=false;
    try { (void)transfer_check_origins(module,before,staged.functions.front(),edits,[]{},64*1024*1024,
            [](std::size_t){throw Exhausted{};}); }
    catch(const Exhausted&) {propagated=true;}
    require(propagated,"shared byte-budget exhaustion was swallowed");
}
} // namespace
int main() {try {actual_scoped_sources();composite_flags();std::cout<<"typed check binding origins: ok\n";return 0;}
catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
