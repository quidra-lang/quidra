#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "ir/mutable_operands.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::optimizer;
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
Function managed_helper(){
    Function f;f.name="helper";f.result=Type::simple(TypeKind::String);
    f.parameters={{"owned",f.result},{"borrowed",f.result,false,true},{"address",f.result,true}};
    SourceLocation callee_site;callee_site.source_file="helper";
    f.blocks={{"entry",{callee_site,StatementBoundary{1},DeclareReference{"ref",f.result},BindReference{"ref",77},LoadLocal{std::numeric_limits<ValueId>::max(),"owned",f.result,true},ConstantBool{42,true},StatementBoundary{1},Branch{42,"yes","no"}}},
              {"yes",{Return{std::numeric_limits<ValueId>::max(),f.result}}},
              {"no",{ConstantInt{31,"7",Type::simple(TypeKind::Int32)},Exit{31}}}};
    f.blocks[0].instructions.insert(f.blocks[0].instructions.begin()+3,AddressLocal{77,"address"});
    return f;
}
Function caller(){
    Function f;f.name="caller";f.entrypoint=true;f.result=Type::simple(TypeKind::String);
    SourceLocation caller_site;caller_site.source_file="caller";
    f.blocks={{"entry",{caller_site,StatementBoundary{1},ConstantString{1,"x"},Clone{2,1,f.result},DeclareLocal{"raw",f.result,"raw",0,0},AddressLocal{3,"raw"},
        Call{4,"helper",{{2,{}},{1,{}},{0,3}},f.result},Return{4,f.result}}}};for(ValueId id=100;id<130;++id)f.blocks[0].instructions.insert(f.blocks[0].instructions.begin()+1,ConstantBool{id,true});f.next_value=130;return f;
}
struct Probe:MutableOperandVisitor{std::set<ValueId>defs,uses;void definition(ValueId&v)override{defs.insert(v);}void use(ValueId&v)override{if(v)uses.insert(v);}};
int main(){
    Module m;m.functions={caller(),managed_helper()};auto rules=RuleRegistry::build(m);
    auto r=BudgetedInliner::run(m,rules);if(!r.inlined_calls)for(const auto&x:r.unavailable)std::cerr<<x.reason<<"\n";require(r.inlined_calls==1,"managed CFG is actually inlined");
    const auto& f=m.functions[0];require(f.inline_scopes.size()==1,"one explicit owned inline frame");
    const auto& s=f.inline_scopes[0];require(s.slots.size()==4,"parameters and pinned reference retain distinct cleanup slots");
    require(s.slots[0].kind==InlineSlotKind::OwnedLocal && s.slots[1].kind==InlineSlotKind::BorrowedLocal && s.slots[2].kind==InlineSlotKind::RawAddress,"actual owned/borrowed/raw address ABI retained");
    std::set<std::uint64_t> statements;bool raw=false,returned=false,exited=false;Probe p;
    for(const auto& b:f.blocks){for(std::size_t i=0;i<b.instructions.size();++i){auto n=b.instructions[i];mutate_operands(n,p);
        if(const auto*q=std::get_if<StatementBoundary>(&n))statements.insert(q->identity);
        if(const auto*q=std::get_if<InlineBindAddress>(&n))raw=q->address==3;
        if(std::holds_alternative<InlineExit>(n)){require(i&&std::holds_alternative<InlineScopeCleanup>(b.instructions[i-1]),"inline process exit cleans only callee scope");exited=true;}
        if(std::holds_alternative<InlineScopeEnd>(n)){require(i&&std::holds_alternative<InlineScopeCleanup>(b.instructions[i-1]),"normal return cleanup precedes scope end");returned=true;}
    }}
    require(statements.size()==2&&s.statements.size()==1&&s.statements.front().original==1&&s.statements.front().remapped!=1,"repeated semantic markers reuse one fresh call-site identity");
    require(raw&&returned&&exited,"all actual CFG and reference binding paths retained");
    require(!p.defs.contains(std::numeric_limits<ValueId>::max()),"high sparse callee ID renamed without max-ID storage");
    for(auto v:p.uses)require(p.defs.contains(v),"every copied actual use maps to a caller definition");
    Module rec;Function self;self.name="self";self.blocks={{"entry",{Call{0,"self",{},Type::simple(TypeKind::Void)},ReturnVoid{}}}};rec.functions={self};auto rr=RuleRegistry::build(rec);
    require(BudgetedInliner::run(rec,rr).inlined_calls==0,"recursive SCC falls back");
    Module parallel_cycle;Function parent;parent.name="parallel_parent";ParallelBlock region;region.out=1;region.table_name="nodes";region.nodes={{"parallel_worker",{}, {}, {},false,false}};parent.blocks={{"entry",{region,ReturnVoid{}}}};Function worker;worker.name="parallel_worker";worker.parallel_node=ParallelNodeMetadata{"worker_wrapper",{},false};worker.blocks={{"entry",{Call{0,parent.name,{},worker.result},ReturnVoid{}}}};Function root;root.name="parallel_caller";root.entrypoint=true;Block block{"entry",{}};for(ValueId id=1;id<20;++id)block.instructions.push_back(ConstantBool{id,true});block.instructions.push_back(Call{0,parent.name,{},root.result});block.instructions.push_back(ReturnVoid{});root.blocks.push_back(std::move(block));parallel_cycle.functions={root,parent,worker};auto pr=RuleRegistry::build(parallel_cycle);
    require(BudgetedInliner::run(parallel_cycle,pr).inlined_calls==0,"parallel node call edges participate in recursion refusal");
    Module budget;budget.functions={caller(),managed_helper()};auto br=RuleRegistry::build(budget);InliningOptions off;off.maximum_operations=0;
    require(BudgetedInliner::run(budget,br,off).inlined_calls==0,"operation exhaustion preserves original call");
    Module nested;auto leaf=managed_helper();leaf.name="leaf";auto mid=caller();mid.name="middle";mid.entrypoint=false;for(auto& n:mid.blocks[0].instructions)if(auto*q=std::get_if<Call>(&n))q->callee="leaf";auto top=caller();for(auto& n:top.blocks[0].instructions)if(auto*q=std::get_if<Call>(&n))*q=Call{4,"middle",{},top.result};mid.parameters.clear();
    for(ValueId id=200;id<240;++id)top.blocks[0].instructions.insert(top.blocks[0].instructions.begin()+1,ConstantBool{id,true});Function reserve;reserve.name="unrelated";for(ValueId id=1;id<=100;++id)reserve.blocks.push_back({"unused"+std::to_string(id),{ConstantBool{id,true},ReturnVoid{}}});top.blocks[0].instructions.insert(top.blocks[0].instructions.end()-1,Call{5,"leaf",{{2,{}},{1,{}},{0,3}},top.result});std::get<Return>(top.blocks[0].instructions.back()).value=5;nested.functions={top,mid,leaf,reserve};auto nr=RuleRegistry::build(nested);auto nrep=BudgetedInliner::run(nested,nr);
    require(nrep.inlined_calls>=2,"nested direct CFG mechanism executes");
    std::set<std::uint64_t> scope_ids,statement_ids;
    for(const auto& scope:nested.functions[0].inline_scopes){require(scope_ids.insert(scope.identity).second,"nested scope identities unique");if(scope.callee=="leaf"&&!scope.parent)require(scope.caller_location&&scope.caller_location->source_file=="caller","later caller continuation keeps opaque caller attribution");if(scope.parent){const auto& scopes=nested.functions[0].inline_scopes;const auto p=std::find_if(scopes.begin(),scopes.end(),[&](const auto& x){return x.identity==scope.parent;});require(p!=scopes.end()&&std::any_of(p->slots.begin(),p->slots.end(),[](const auto& x){return x.name.ends_with("result");}),"nested return result is owned by the enclosing frame");}}
    for(const auto& scope:nested.functions[0].inline_scopes)for(const auto& statement:scope.statements)require(statement.remapped!=1&&statement_ids.insert(statement.remapped).second,"different inline statements have distinct remapped identities");
    return 0;
}
