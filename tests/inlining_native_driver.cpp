// Real checked/lowered modules and backend output for the native inlining gate.
// Native toolchain orchestration supplies artifact paths for the explicit pass.
#include "quidra/compiler.hpp"
#include "quidra/frontend.hpp"
#include "quidra/lowering.hpp"
#include "quidra/llvm_backend.hpp"
#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "ir/analysis/call_graph.hpp"
#include "toolchain/call_site_table.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <set>
#include <map>
#include <tuple>

using namespace quidra;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void text(const std::filesystem::path& path,const std::string& value){std::ofstream f(path);f.exceptions(std::ios::failbit|std::ios::badbit);f<<value;}
void save_sites(const std::filesystem::path& path,const CallSiteInfo& info){
    // The linked table exposes each owner as a contiguous first/count range.
    // Checking that invariant here catches interleaved virtual-frame records.
    std::map<std::uint32_t,std::tuple<std::size_t,std::size_t,std::size_t>> ranges;
    for(std::size_t id=1;id<info.sites.size();++id){
        const auto owner=info.sites[id].owner;require(owner<info.functions.size(),"call site owner must exist");
        auto [it,inserted]=ranges.try_emplace(owner,id,id,0);(void)inserted;
        std::get<1>(it->second)=id;++std::get<2>(it->second);
    }
    for(const auto& [owner,range]:ranges){(void)owner;require(std::get<1>(range)-std::get<0>(range)+1==std::get<2>(range),"logical call site ranges must be contiguous");}
    std::ofstream f(path);f.exceptions(std::ios::failbit|std::ios::badbit);
    f<<info.library<<' '<<info.debug<<' '<<info.sites.size()<<' '<<info.functions.size()<<'\n';
    for(const auto& s:info.sites)f<<s.file<<' '<<s.line<<' '<<s.column<<' '<<s.callee<<' '<<s.owner<<' '<<unsigned(s.callee_kind)<<' '<<unsigned(s.flags)<<'\n';
    for(const auto& fn:info.functions){f<<std::quoted(fn.display)<<' '<<unsigned(fn.kind)<<' '<<unsigned(fn.flags)<<' '<<fn.symbols.size()<<'\n';for(const auto& s:fn.symbols)f<<std::quoted(s)<<'\n';}
}
CallSiteInfo load_sites(const std::filesystem::path& path){
    std::ifstream f(path);f.exceptions(std::ios::failbit|std::ios::badbit);CallSiteInfo info;std::size_t sites,functions;
    f>>info.library>>info.debug>>sites>>functions;info.sites.clear();
    for(std::size_t i=0;i<sites;++i){CallSite s;unsigned kind,flags;f>>s.file>>s.line>>s.column>>s.callee>>s.owner>>kind>>flags;s.callee_kind=static_cast<CallSiteCallee>(kind);s.flags=static_cast<std::uint8_t>(flags);info.sites.push_back(s);}
    for(std::size_t i=0;i<functions;++i){CallSiteFunction fn;unsigned kind,flags;std::size_t count;f>>std::quoted(fn.display)>>kind>>flags>>count;fn.kind=static_cast<CallSiteFunctionKind>(kind);fn.flags=static_cast<std::uint8_t>(flags);for(std::size_t j=0;j<count;++j){std::string s;f>>std::quoted(s);fn.symbols.push_back(std::move(s));}info.functions.push_back(std::move(fn));}return info;
}
void emit(const std::filesystem::path& root,const std::filesystem::path& output,bool expect_node,const std::string& target,bool expect_nested=false){
    // Public compiler API uses the actual loader, generic expansion and checker.
    auto checked=check_file(root,{},root.parent_path());
    auto module=ir::lower(checked);require(module.summaries!=nullptr,"actual lowering must attach owned semantic summaries");
    auto baseline=emit_native_llvm(module,{});text(output.string()+".baseline.ll",baseline.text);save_sites(output.string()+".baseline.sites",baseline.call_sites);
    std::set<std::string> target_irs;std::optional<std::string> target_declaration;
    for(const auto& fn:module.functions)if(fn.declaration && fn.declaration->source_name==target && fn.declaration->kind==DeclarationIdentity::Kind::Function){
        const auto key=declaration_key(*fn.declaration);
        require(!target_declaration || *target_declaration==key,"fixture original declaration must be unambiguous");target_declaration=key;
        if(!fn.shape_wrapper)target_irs.insert(fn.name);
    }
    require(!target_irs.empty(),"native target body must come from an actual original checked declaration");
    std::set<std::tuple<std::string,std::uint32_t,std::uint32_t>> original_calls;
    for(const auto& fn:module.functions)for(const auto& block:fn.blocks)for(const auto& instruction:block.instructions)
        if(const auto* call=std::get_if<ir::Call>(&instruction))original_calls.emplace(call->callee,call->position.line,call->position.column);
    auto registry=optimizer::RuleRegistry::build(module);
    optimizer::InliningOptions options;const auto modes=ir::analysis::node_cleanup_functions(module);options.node_cleanup_modes.emplace();
    for(const auto& fn:module.functions){options.node_cleanup_modes->emplace(fn.name,modes.contains(fn.name));
        for(std::size_t b=0;b<fn.blocks.size();++b)for(std::size_t i=0;i<fn.blocks[b].instructions.size();++i)
            if(std::holds_alternative<ir::Call>(fn.blocks[b].instructions[i]))options.hot_sites.push_back({fn.name,b,i});}
    auto result=optimizer::BudgetedInliner::run(module,registry,std::move(options));
    for(const auto& u:result.unavailable)std::cerr<<u.caller<<" -> "<<u.callee<<": "<<u.reason<<'\n';
    require(result.inlined_calls>0,"native gate must really inline checked code");
    std::size_t targets=0,target_nodes=0,target_ordinary=0,nested=0,nodes=0,ordinary=0,owned=0,borrowed=0,raw=0,pinned=0,cleanup=0,exit=0,fail=0;
    for(const auto& fn:module.functions){for(const auto& s:fn.inline_scopes){require(original_calls.contains({s.callee,s.position.line,s.position.column}),"inline edge must retain exact removed call position");
        if(target_irs.contains(s.callee)){targets++;(s.node_cleanup?target_nodes:target_ordinary)++;}
        nested+=s.parent!=0;(s.node_cleanup?nodes:ordinary)++;
        for(const auto& slot:s.slots)switch(slot.kind){case ir::InlineSlotKind::OwnedLocal:owned++;break;case ir::InlineSlotKind::BorrowedLocal:borrowed++;break;case ir::InlineSlotKind::RawAddress:raw++;break;case ir::InlineSlotKind::PinnedReference:pinned++;break;}}
        for(const auto& b:fn.blocks)for(const auto& n:b.instructions){cleanup+=std::holds_alternative<ir::InlineScopeCleanup>(n);exit+=std::holds_alternative<ir::InlineExit>(n);fail+=std::holds_alternative<ir::FailError>(n);}}
    require(targets>0,"requested user helper must actually be inlined, not just unrelated library code");
    require(!expect_nested || nested>0,"nested regression must copy an actual child inline scope");
    require(expect_node?target_nodes>0:target_ordinary>0,"scope cleanup mode must come from the typed node call graph");
    require(cleanup>0,"native module must contain actual scope cleanup instructions");
    auto inlined=emit_native_llvm(module,{});text(output.string()+".inlined.ll",inlined.text);save_sites(output.string()+".inlined.sites",inlined.call_sites);
    std::cout<<"inlined="<<result.inlined_calls<<" nested="<<nested<<" nodes="<<nodes<<" ordinary="<<ordinary<<" owned="<<owned<<" borrowed="<<borrowed<<" raw="<<raw<<" pinned="<<pinned<<" cleanup="<<cleanup<<" exit="<<exit<<" fail="<<fail<<'\n';
}
}
int main(int argc,char** argv){try{
    if((argc==6 || (argc==7 && std::string_view(argv[6])=="nested")) && std::string_view(argv[1])=="emit"){emit(argv[2],argv[3],std::string_view(argv[4])=="node",argv[5],argc==7);return 0;}
    if(argc==5 && std::string_view(argv[1])=="table"){toolchain::write_call_site_table(argv[3],load_sites(argv[2]),argv[4]);return 0;}
    throw std::runtime_error("expected emit SOURCE PREFIX ordinary|node TARGET [nested], or table SITES OBJECT OUTPUT");
}catch(const CompileErrors& e){for(const auto& d:e.diagnostics())std::cerr<<d.code<<" at "<<d.span.start.line<<":"<<d.span.start.column<<": "<<d.message<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
