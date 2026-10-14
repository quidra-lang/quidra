#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "quidra/optimizer.hpp"
#include "ir/canonical_encoding.hpp"
#include <cstdio>
#include <stdexcept>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::optimizer;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Function body(std::string name,std::size_t count){
    Function f;f.name=std::move(name);f.result=Type::simple(TypeKind::Void);
    Block block{"authored",{}};
    for(std::size_t i=0;i<count;++i)block.instructions.push_back(ConstantBool{static_cast<ValueId>(i+1),true});
    block.instructions.push_back(ReturnVoid{});f.blocks.push_back(std::move(block));return f;
}
Module example(){
    Module m;auto caller=body("actual_caller",650);caller.entrypoint=true;
    auto& instructions=caller.blocks[0].instructions;
    instructions.pop_back();
    for(const auto& callee:{"tiny","hot_parent","hot_sibling","cold_other"})
        instructions.push_back(Call{0,callee,{},Type::simple(TypeKind::Void)});
    instructions.push_back(ReturnVoid{});
    auto parent=body("hot_parent",105);
    parent.blocks[0].instructions.insert(parent.blocks[0].instructions.end()-1,
        Call{0,"nested_candidate",{},Type::simple(TypeKind::Void)});
    m.functions={caller,body("tiny",0),parent,body("nested_candidate",105),body("hot_sibling",105),body("cold_other",105)};
    return m;
}
std::size_t calls(const Function& fn,const std::string& name){
    std::size_t count=0;for(const auto& block:fn.blocks)for(const auto& instruction:block.instructions)
        if(const auto* call=std::get_if<Call>(&instruction);call&&call->callee==name)++count;
    return count;
}
std::string function_fields(const Module& module){
    // Full typed fields, including attribution and allocation metadata, so a
    // discarded staged run cannot quietly publish scopes, entries or edits.
    canonical_detail::Encoder encoder(true);encoder.put(module.functions);
    return encoder.finish().bytes;
}
void pipeline_modes(){
    OptimizerOptions disabled;disabled.exact_binding=DeclarationBindingMode::On;
    disabled.interproc=OptimizerMode::Off;disabled.inline_functions=OptimizerMode::Off;
    const auto baseline=optimize(example(),disabled);
    require(calls(baseline.functions[0],"tiny")==1 && baseline.functions[0].inline_scopes.empty(),
        "explicit off preserves the real eligible tiny call in the existing pipeline fixture");
    const auto unchanged=function_fields(baseline);
    OptimizerOptions defaults;defaults.exact_binding=DeclarationBindingMode::On;
    const auto ordinary=optimize(example(),defaults);
    require(!calls(ordinary.functions[0],"tiny") && !ordinary.functions[0].inline_scopes.empty(),
        "unspecified options retain actual exact-binding On pipeline inlining");
    auto independent=defaults;independent.interproc=OptimizerMode::Off;independent.inline_functions=OptimizerMode::On;
    require(!calls(optimize(example(),independent).functions[0],"tiny"),
        "explicit inline On remains independent of helper specialization Off");
    auto inline_off=defaults;inline_off.inline_functions=OptimizerMode::Off;
    require(function_fields(optimize(example(),inline_off))==unchanged,
        "explicit inline Off overrides the exact-binding On default without changing fields");
    auto inline_verify=defaults;inline_verify.inline_functions=OptimizerMode::Verify;
    require(function_fields(optimize(example(),inline_verify))==unchanged,
        "inline Verify stages the actual eligible expansion without publishing any fields");
    auto facts_off=defaults;facts_off.cfg_facts=OptimizerMode::Off;
    require(function_fields(optimize(example(),facts_off))==unchanged,
        "cfg Off prevents dependent helper and inline rewrites under exact-binding On");
    auto facts_verify=defaults;facts_verify.cfg_facts=OptimizerMode::Verify;
    require(function_fields(optimize(example(),facts_verify))==unchanged,
        "cfg Verify keeps dependent staged helper and inline work observational");
}
}
int main(){try{
    auto module=example();auto rules=RuleRegistry::build(module);InliningOptions options;
    options.hot_sites={{"actual_caller",0,651},{"actual_caller",0,652}};
    const auto report=BudgetedInliner::run(module,rules,options);
    require(report.inlined_calls==4,"real hot call sites survive an earlier split and propagate into copied candidate calls");
    const auto& caller=module.functions[0];
    require(!calls(caller,"tiny")&&!calls(caller,"hot_parent")&&!calls(caller,"hot_sibling")&&!calls(caller,"nested_candidate"),
        "both later hot calls and inherited nested candidate are expanded");
    require(calls(caller,"cold_other")==1,"a nearby cold call does not inherit a shifted hot coordinate");
    require(calls(module.functions[2],"nested_candidate")==1,"a copied candidate does not mark its original cold declaration hot");
    auto invalid=example();auto invalid_rules=RuleRegistry::build(invalid);options.hot_sites={{"actual_caller",0,0}};
    require(!BudgetedInliner::run(invalid,invalid_rules,options).changed(),"a non-call hot coordinate is rejected before rewriting");
    pipeline_modes();
    std::puts("Inlining hot sites and explicit pipeline modes verified");
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
