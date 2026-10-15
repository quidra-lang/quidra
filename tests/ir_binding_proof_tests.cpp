#include "ir/analysis/binding_proof.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
using namespace quidra;using namespace quidra::ir;using namespace quidra::ir::analysis;
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
}
int main(){try{
    const auto leaf=Type::class_type("renamed_leaf"),model=Type::class_type("renamed_model"),holder=Type::class_type("renamed_holder");
    const auto list=Type::array(leaf);const auto integer=Type::simple(TypeKind::Int64);
    Module module;
    module.classes={{"renamed_leaf",{},{}},{"renamed_model",{"first","second"},{leaf,leaf}},
        {"renamed_holder",{"bound"},{list}}};
    Function construct;construct.name="opaque_ctor_ir";construct.result=holder;
    construct.parameters={{"input",model,false,true}};
    construct.blocks.emplace_back(Block{"entry",{}});auto& code=construct.blocks.back().instructions;
    code.emplace_back(LoadLocal{1,"input",model});code.emplace_back(FieldGet{2,1,0,leaf});code.emplace_back(FieldGet{3,1,1,leaf});
    code.emplace_back(ArrayMake{4,{},list});code.emplace_back(StoreLocal{"acc",4,list,true,true});
    code.emplace_back(LoadLocal{5,"acc",list});code.emplace_back(ArrayLength{6,5});code.emplace_back(ArrayGrowMove{7,5,list});
    code.emplace_back(ArraySet{7,6,2,leaf,{},true,true});code.emplace_back(StoreLocal{"acc",7,list,true,true});
    code.emplace_back(LoadLocal{8,"acc",list});code.emplace_back(ArrayLength{9,8});code.emplace_back(ArrayGrowMove{10,8,list});
    code.emplace_back(ArraySet{10,9,3,leaf,{},true,true});code.emplace_back(StoreLocal{"acc",10,list,true,true});
    code.emplace_back(LoadLocal{11,"acc",list});code.emplace_back(ClassMake{12,holder,{}});
    code.emplace_back(FieldSet{12,0,11,list});code.emplace_back(Return{12,holder});
    DeclarationIdentity origin;origin.kind=DeclarationIdentity::Kind::Constructor;origin.package="generic";origin.module_path="proof";origin.owner_class="Holder";origin.source_name="0";
    construct.declaration=origin;module.functions.push_back(construct);
    BindingSources sources;
    ClassIdentity leaf_id{"generic","proof","Leaf",{}},model_id{"generic","proof","Model",{}},holder_id{"generic","proof","Holder",{}};
    sources.classes.emplace("renamed_leaf",BindingClass{leaf_id,{},true});
    sources.classes.emplace("renamed_model",BindingClass{model_id,{{"first",leaf,true,false,false},{"second",leaf,true,false,false}},true});
    sources.classes.emplace("renamed_holder",BindingClass{holder_id,{{"bound",list,false,true,false}},true});
    FunctionBindingSource function;function.declaration=origin;function.complete=true;function.semantic_parameter_for_ir_parameter={0};
    function.validated_initialization[0]={{{semantics::PathStep::field_named("first")}},{{semantics::PathStep::field_named("second")}}};
    function.collections.push_back(CollectionOrigin{{0,18},11,1,leaf,{{semantics::PathStep::field_named("first")},{semantics::PathStep::field_named("second")}},true});
    sources.functions.emplace(construct.name,function);
    ModuleSummaries summaries;
    auto proof=binding_proof(module,summaries,sources);
    if (proof.collections.size()==1 && !proof.collections[0].available) {
        const auto& c=proof.collections[0];
        const auto& array=proof.objects[0].at({0,18},11);
        std::fprintf(stderr,"binding closure: values=%zu elements=%zu available=%d paths=%d initialized=%d; array.closed=%d array.available=%d array.elements=%zu; gaps:",
            c.values.size(),c.elements.size(),c.available,c.paths_available,c.initialized,
            array.array_closed,array.available,array.elements.size());
        for(const auto why:c.gaps) std::fprintf(stderr," %u",static_cast<unsigned>(why));
        std::fprintf(stderr,"\n");
    }
    require(proof.collections.size()==1 && proof.collections[0].available && proof.collections[0].elements.size()==2,
        "actual fixed grow/set list closes through readonly borrowed parameter paths");
    require(proof.bound_fields.size()==1 && proof.bound_fields[0].constructor_or_rebind_only && proof.bound_fields[0].available,
        "private bound field has exact constructor-only writer and fixed element paths");
    require(proof.objects[0].at({0,18},12).allocation && proof.objects[0].at({0,18},12).allocation->at==InstrRef{0,16},
        "class allocation identity is its actual site, independent of tensor alias classes");
    require(proof.constructor_allocations.at(construct.name),"actual constructor body return certifies allocation-site birth, not role alone");
    auto cached=binding_proof(module,summaries,sources,proof.objects);
    require(cached.collections[0].available && cached.bound_fields[0].available,"cached object observations preserve proofs without rebuilding analyses");
    auto saved=module.functions[0];
    // Copied body retains the exact callee owner through live scope membership.
    auto& inlined=module.functions[0];inlined.name="arbitrary_caller";
    auto caller_origin=origin;caller_origin.kind=DeclarationIdentity::Kind::Function;caller_origin.owner_class.clear();caller_origin.source_name="caller";inlined.declaration=caller_origin;
    InlineScope scope;scope.identity=41;scope.callee=construct.name;scope.blocks={"entry"};inlined.inline_scopes.push_back(scope);
    auto caller_source=function;caller_source.declaration=caller_origin;sources.functions.emplace(inlined.name,caller_source);
    auto inline_proof=binding_proof(module,summaries,sources);
    require(inline_proof.bound_fields[0].constructor_or_rebind_only && inline_proof.bound_fields[0].writers.back().semantic_function==construct.name,
        "inline field writer retains constructor declaration rather than ordinary caller spelling");
    sources.functions.erase(inlined.name);module.functions[0]=saved;
    // A known body can mutate class fields even when it is not opaque.
    semantics::EffectSummary mutation;mutation.exposed_writes.resize(1);
    mutation.exposed_writes[0].insert({semantics::StorageRoot::parameter(0),{semantics::PathStep::field_named("first")}});
    ModuleSummaries mutating;FunctionSummary call_summary;call_summary.semantic=&mutation;call_summary.effects_complete=true;call_summary.effects=0;call_summary.semantic_parameter_for_ir_parameter={0};mutating.by_function.emplace("actual_mutator",call_summary);
    auto& stale=module.functions[0].blocks[0].instructions;stale.insert(stale.begin()+3,Call{0,"actual_mutator",{{1,{}}},Type::simple(TypeKind::Void)});
    sources.functions.at(construct.name).collections[0].at.index=19;
    auto invalidated=binding_proof(module,mutating,sources);
    require(!invalidated.collections[0].available,"known mutating call cannot preserve stale field values or initialization");
    module.functions[0]=saved;sources.functions.at(construct.name).collections[0].at.index=18;
    auto& opaque=module.functions[0].blocks[0].instructions;opaque.insert(opaque.begin()+18,Call{0,"external_unknown",{},Type::simple(TypeKind::Void)});
    sources.functions.at(construct.name).collections[0].at.index=19;
    require(!binding_proof(module,summaries,sources).collections[0].available,"opaque call cannot leave a previously closed list valid");
    module.functions[0]=saved;sources.functions.at(construct.name).collections[0].at.index=18;
    Function other;other.name="different_generated_method";other.result=Type::simple(TypeKind::Void);other.parameters={{"owner",holder,true}};
    other.blocks={{"entry",{LoadLocal{1,"owner",holder},ArrayMake{2,{},list},FieldSet{1,0,2,list},ReturnVoid{}}}};
    auto method_origin=origin;method_origin.kind=DeclarationIdentity::Kind::Method;method_origin.source_name="reset";method_origin.mutation_marker=true;other.declaration=method_origin;
    module.functions.push_back(other);FunctionBindingSource writer;writer.declaration=method_origin;writer.complete=true;writer.receiver_ir_parameter=0;writer.semantic_parameter_for_ir_parameter={0};sources.functions.emplace(other.name,writer);
    auto ordinary=binding_proof(module,summaries,sources);
    require(!ordinary.bound_fields[0].constructor_or_rebind_only,"ordinary exact method writer prevents class binding closure");
    sources.functions.at(other.name).rebind=true;
    auto rebound=binding_proof(module,summaries,sources);
    require(rebound.bound_fields[0].constructor_or_rebind_only,"explicit exact rebind role is accepted without recognizing generated spelling");
    module.functions.resize(1);sources.functions.erase(other.name);
    auto& mutated=module.functions[0].blocks[0].instructions;
    mutated.insert(mutated.begin()+16,Clone{20,11,list});
    sources.functions.at(construct.name).collections[0].at.index=19;
    auto aliased=binding_proof(module,summaries,sources);
    require(!aliased.collections[0].available,"array alias before publication cannot authorize a fixed binding proof");
    module.functions[0]=saved;sources.functions.at(construct.name).collections[0].at.index=18;
    auto& dynamic=module.functions[0].blocks[0].instructions;
    std::get<ArraySet>(dynamic[8]).index=1;
    require(!binding_proof(module,summaries,sources).collections[0].available,"dynamic/noninteger array index has no fixed contents proof");
    module.functions[0]=saved;
    auto& exposed=module.functions[0].blocks[0].instructions;exposed.insert(exposed.begin()+16,AddressLocal{30,"acc"});
    sources.functions.at(construct.name).collections[0].at.index=19;
    require(!binding_proof(module,summaries,sources).collections[0].available,"array local address exposure invalidates structural use closure");
    module.functions[0]=saved;sources.functions.at(construct.name).collections[0].at.index=18;
    sources.functions.at(construct.name).collections.clear();
    auto missing=binding_proof(module,summaries,sources);
    require(missing.collections.empty() && !missing.bound_fields[0].available,"absent exact collection origin is not guessed from array shape or names");
    std::puts("IR binding proofs: exact writers, fixed collections, object sites and alias rejection verified");
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
