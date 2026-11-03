#include "optimizer/helper_specialization.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "ir/analysis/summary_facts.hpp"
#include "platform/sha256.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::facts;
using namespace quidra::ir::analysis;
using namespace quidra::optimizer;
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
const Function& find(const Module& m,const std::string& n){for(const auto& f:m.functions)if(f.name==n)return f;throw std::runtime_error("missing function");}
DeclarationIdentity identity(std::string name){DeclarationIdentity d;d.package="generic";d.module_path="helpers";d.source_name=std::move(name);d.signature="(tensor<real32>)bool";d.parameters={{"x","tensor<real32>",false,false}};d.result_type="bool";return d;}
Module program(){
    const auto tensor=Type::tensor(Type::simple(TypeKind::Real32),2,{9,9});
    Function helper;helper.name="helper";helper.declaration=identity("helper");helper.parameters={{"x",tensor,false,true}};helper.result=Type::simple(TypeKind::Bool);
    helper.blocks={{"entry",{LoadLocal{std::numeric_limits<ValueId>::max(),"x",tensor},TensorIsTracked{2,std::numeric_limits<ValueId>::max()},Return{2,helper.result}}}};
    Function caller;caller.name="caller";caller.entrypoint=true;
    caller.blocks={{"entry",{ConstantInt{1,"3",Type::simple(TypeKind::Int64)},ArrayMake{2,{1},Type::array(Type::simple(TypeKind::Int64),1)},
        TensorCreate{3,2,{},tensor,tensor_fill_mode::zeros},Call{4,"helper",{{3,{}}},helper.result},
        TensorCreate{5,2,{},tensor,tensor_fill_mode::zeros},TensorTrack{6,5,{},tensor,tensor_track_mode::track},Call{7,"helper",{{6,{}}},helper.result},ConstantBool{8,false},ReturnVoid{}}}};
    Module m;m.functions={helper,caller};m.declaration_catalog={*helper.declaration};return m;
}
}
int main(){try{
    auto m=program();AnalysisCache cache(m);auto r=HelperSpecialization::run(m,cache);
    require(r.generated_clones==2 && r.rewritten_calls==2,"disagreeing proven caller tracking facts require two bounded clones");
    const auto& a=std::get<Call>(m.functions[1].blocks[0].instructions[3]);
    const auto& b=std::get<Call>(m.functions[1].blocks[0].instructions[6]);
    const auto& fa=find(m,a.callee);const auto& fb=find(m,b.callee);
    require(a.callee!=b.callee && fa.declaration && same_declaration(*fa.declaration,*m.functions[0].declaration),"clones retain exact original identity");
    require(std::holds_alternative<ConstantBool>(fa.blocks[0].instructions[1]) && !std::get<ConstantBool>(fa.blocks[0].instructions[1]).value,"proven false tracking folds inside the matching clone");
    require(std::holds_alternative<TensorIsTracked>(fb.blocks[0].instructions[1]),"true tracking remains dynamic");
    require(r.entries.at(a.callee)->tensors.at(0).type.length==1 && r.entries.at(a.callee)->tensors.at(0).extents[0].known()==3,"caller-validated geometry replaces an unvalidated signature");
    require(r.entries.at("helper")->origin==EntryFactOrigin::AllDirectCallSites && r.entries.at(a.callee)->origin==EntryFactOrigin::SpecializedCalls,"whole-call meets and clone obligations remain distinct");
    require(!r.entries.at(a.callee)->tensors.at(0).owns_storage && r.entries.at(a.callee)->tensors.at(0).unique==Tri::Unknown,"entry facts never promote storage ownership");
    cache.invalidate_all();auto again=HelperSpecialization::run(m,cache,r.entries);
    require(again.rewritten_calls==0 && again.folded_tracking==0 && again.generated_clones==0,"typed prior metadata makes rerun idempotent");
    auto shifted=program();shifted.functions[0].source_file="other";shifted.functions[0].source_line=900;shifted.functions[0].declaration->source_file="provenance";AnalysisCache shifted_cache(shifted);
    auto rr=HelperSpecialization::run(shifted,shifted_cache);require(rr.clones[0].clone==r.clones[0].clone && rr.clones[1].clone==r.clones[1].clone,"fact names ignore source positions and provenance");
    auto limited=program();AnalysisCache limited_cache(limited);SpecializationOptions options;options.maximum_clones=1;auto lr=HelperSpecialization::run(limited,limited_cache,{},options);
    require(lr.generated_clones==1 && lr.budget_fallbacks!=0 && limited.functions.size()==3,"clone exhaustion retains source fallback");
    auto opaque=program();opaque.functions[1].blocks[0].instructions.insert(opaque.functions[1].blocks[0].instructions.begin(),FunctionRef{90,"helper",Type::function(Type::simple(TypeKind::Bool),{opaque.functions[0].parameters[0].type})});AnalysisCache opaque_cache(opaque);
    auto orr=HelperSpecialization::run(opaque,opaque_cache);require(!orr.entries.contains("helper"),"address-taken helper never receives an all-call-site theorem");
    auto unknown=program();unknown.functions[1].parameters={{"x",unknown.functions[0].parameters[0].type,false,true}};unknown.functions[1].blocks={{"entry",{LoadLocal{std::numeric_limits<ValueId>::max(),"x",unknown.functions[0].parameters[0].type},Call{4,"helper",{{std::numeric_limits<ValueId>::max(),{}}},unknown.functions[0].result},ReturnVoid{}}}};AnalysisCache unknown_cache(unknown);
    auto ur=HelperSpecialization::run(unknown,unknown_cache);require(ur.generated_clones==0 && ur.entries.at("helper")->tensors.at(0).type.length<0,"declared caller geometry cannot prove itself");
    unknown.functions[1].parameters.push_back({"n",Type::simple(TypeKind::Nat64)});
    ShapedConstraintCheck guard;guard.value=std::numeric_limits<ValueId>::max();guard.extents={1};
    unknown.functions[1].blocks={{"entry",{LoadLocal{std::numeric_limits<ValueId>::max(),"x",unknown.functions[0].parameters[0].type},LoadLocal{1,"n",Type::simple(TypeKind::Nat64)},guard,Call{4,"helper",{{std::numeric_limits<ValueId>::max(),{}}},unknown.functions[0].result},ReturnVoid{}}}};
    AnalysisCache symbol_cache(unknown);auto sr=HelperSpecialization::run(unknown,symbol_cache);const auto& sf=sr.entries.at("helper")->tensors.at(0);
    require(sf.type.length==1 && sf.extents[0].kind==ShapeExtent::Kind::Unknown && sf.extents[0].symbols.empty(),"caller-local shape symbols never escape into entry metadata");
    auto stale=program();stale.functions[1].blocks[0].instructions.insert(stale.functions[1].blocks[0].instructions.begin()+3,Call{0,"opaque_native",{{3,{}}},Type::simple(TypeKind::Void)});AnalysisCache stale_cache(stale);
    auto cr=HelperSpecialization::run(stale,stale_cache);require(std::get<Call>(stale.functions[1].blocks[0].instructions[4]).callee=="helper" && cr.generated_clones<=1,"opaque descriptor mutation kills stale caller tracking/geometry proofs");
    auto propagated=program();propagated.functions[1].blocks[0].instructions.resize(4);propagated.functions[1].blocks[0].instructions.push_back(ReturnVoid{});
    Function wrapper=propagated.functions[0];wrapper.name="wrapper";wrapper.declaration=identity("wrapper");wrapper.blocks={{"entry",{LoadLocal{11,"x",wrapper.parameters[0].type},Call{12,"helper",{{11,{}}},wrapper.result},Return{12,wrapper.result}}}};
    propagated.functions[1].blocks[0].instructions[3]=Call{4,"wrapper",{{3,{}}},wrapper.result};propagated.functions.push_back(wrapper);AnalysisCache propagate_cache(propagated);
    auto pr=HelperSpecialization::run(propagated,propagate_cache);require(pr.entries.at("helper")->tensors.at(0).tracked==Tri::False && pr.entries.at("helper")->tensors.at(0).type.length==1,"validated facts meet and propagate through actual nested helper calls");
    auto work=program();AnalysisCache work_cache(work);options={};options.maximum_operations=0;auto wr=HelperSpecialization::run(work,work_cache,{},options);require(!wr.changed() && wr.budget_fallbacks!=0 && work.functions.size()==2,"work exhaustion is deterministic conservative fallback");
    std::puts("helper specialization: caller facts, exact clones, sparse values and bounded fallback verified");
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
