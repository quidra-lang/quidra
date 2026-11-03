#include "ir/analysis/loops.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>

namespace quidra::ir::analysis {
namespace {
std::vector<std::vector<BlockId>> components(const Cfg& cfg,const std::vector<BlockId>& group) {
    std::vector<bool> allowed(cfg.succ.size()),seen(cfg.succ.size());
    for(const auto block:group)allowed[block]=true;
    std::vector<BlockId> order;
    struct Frame{BlockId block;std::size_t next{};};std::vector<Frame> stack;
    for(const auto root:group)if(!seen[root]) {
        seen[root]=true;stack.push_back({root});
        while(!stack.empty()) {
            auto& frame=stack.back();const auto& succ=cfg.succ[frame.block];
            if(frame.next==succ.size()){order.push_back(frame.block);stack.pop_back();continue;}
            const auto next=succ[frame.next++];if(allowed[next] && !seen[next]){seen[next]=true;stack.push_back({next});}
        }
    }
    std::fill(seen.begin(),seen.end(),false);std::vector<std::vector<BlockId>> result;
    std::vector<BlockId> pending;
    for(auto it=order.rbegin();it!=order.rend();++it)if(!seen[*it]) {
        result.emplace_back();seen[*it]=true;pending.push_back(*it);
        while(!pending.empty()) {const auto block=pending.back();pending.pop_back();result.back().push_back(block);
            for(const auto pred:cfg.pred[block])if(allowed[pred] && !seen[pred]){seen[pred]=true;pending.push_back(pred);}}
        std::sort(result.back().begin(),result.back().end());
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.front()<b.front();});return result;
}
bool cyclic(const Cfg& cfg,const std::vector<BlockId>& group) {
    return group.size()>1 || (!group.empty() && std::find(cfg.succ[group.front()].begin(),cfg.succ[group.front()].end(),group.front())!=cfg.succ[group.front()].end());
}
bool contains(const std::vector<BlockId>& whole,const std::vector<BlockId>& part) {
    return std::includes(whole.begin(),whole.end(),part.begin(),part.end());
}
} // namespace
LoopNest build_loops(const Cfg& cfg,const DomTree& dom) {
    const auto count=cfg.succ.size();
    if(cfg.pred.size()!=count || cfg.reachable.size()!=count || dom.block_count!=count || dom.scope!=DomScope::EntryPaths)
        throw std::invalid_argument("loop graph/dominator scope dimensions disagree");
    for(std::size_t block=0;block<count;++block) {
        for(const auto next:cfg.succ[block])if(next>=count)throw std::invalid_argument("loop successor is out of range");
        for(const auto pred:cfg.pred[block])if(pred>=count)throw std::invalid_argument("loop predecessor is out of range");
    }
    LoopNest result;result.reachable=cfg.reachable;result.innermost_of_block.assign(count,-1);result.loop_available=cfg.reachable;
    std::map<BlockId,std::set<BlockId>> latches;
    for(BlockId block=0;block<count;++block)if(cfg.reachable[block])for(const auto next:cfg.succ[block])
        if(cfg.reachable[next] && dom.dominates(next,block))latches[next].insert(block);
    for(const auto& [header,tails]:latches) {
        Loop loop;loop.header=header;loop.latches.assign(tails.begin(),tails.end());
        std::set<BlockId> body{header};std::vector<BlockId> pending;
        for(const auto tail:tails)if(body.insert(tail).second)pending.push_back(tail);
        while(!pending.empty()) {const auto block=pending.back();pending.pop_back();for(const auto pred:cfg.pred[block])
            if(cfg.reachable[pred] && body.insert(pred).second && pred!=header){if(!dom.dominates(header,pred))loop.available=false;pending.push_back(pred);}}
        loop.blocks.assign(body.begin(),body.end());std::vector<BlockId> entering;
        for(const auto pred:cfg.pred[header])if(cfg.reachable[pred] && !body.contains(pred))entering.push_back(pred);
        if(header!=0 && entering.size()==1 && cfg.succ[entering.front()].size()==1)loop.preheader=entering.front();
        for(const auto block:loop.blocks)for(const auto next:cfg.succ[block])if(!body.contains(next))loop.exits.push_back({block,next});
        result.loops.push_back(std::move(loop));
    }
    if(result.loops.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw std::length_error("loop index exhausted");
    std::vector<BlockId> reachable;for(BlockId block=0;block<count;++block)if(cfg.reachable[block])reachable.push_back(block);
    std::vector<std::vector<BlockId>> work;if(!reachable.empty())work.push_back(std::move(reachable));
    while(!work.empty()) {
        auto group=std::move(work.back());work.pop_back();
        for(auto component:components(cfg,group))if(cyclic(cfg,component)) {
            std::set<BlockId> members(component.begin(),component.end());std::vector<BlockId> entries;
            for(const auto block:component)if(block==0 || std::any_of(cfg.pred[block].begin(),cfg.pred[block].end(),[&](auto pred){return cfg.reachable[pred] && !members.contains(pred);}))entries.push_back(block);
            const bool reducible=entries.size()==1 && std::all_of(component.begin(),component.end(),[&](auto block){return dom.dominates(entries.front(),block);});
            if(!reducible) {result.irreducible.push_back({component,entries});for(const auto block:component)result.loop_available[block]=false;}
            else {
                component.erase(std::find(component.begin(),component.end(),entries.front()));
                if(!component.empty())work.push_back(std::move(component));
            }
        }
    }
    std::sort(result.irreducible.begin(),result.irreducible.end(),[](const auto& a,const auto& b){return a.blocks<b.blocks;});
    for(auto& loop:result.loops)loop.available &= std::all_of(loop.blocks.begin(),loop.blocks.end(),[&](auto block){return result.loop_available[block];});
    for(std::size_t i=0;i<result.loops.size();++i)for(std::size_t j=i+1;j<result.loops.size();++j) {
        const auto& a=result.loops[i].blocks;const auto& b=result.loops[j].blocks;
        std::vector<BlockId> overlap;std::set_intersection(a.begin(),a.end(),b.begin(),b.end(),std::back_inserter(overlap));
        if(!overlap.empty() && !contains(a,b) && !contains(b,a)){result.loops[i].available=false;result.loops[j].available=false;}
    }
    for(std::size_t i=0;i<result.loops.size();++i) {
        auto& loop=result.loops[i];std::optional<std::uint32_t> parent;
        for(std::size_t j=0;j<result.loops.size();++j)if(result.loops[j].blocks.size()>loop.blocks.size() && contains(result.loops[j].blocks,loop.blocks) &&
            (!parent || result.loops[j].blocks.size()<result.loops[*parent].blocks.size()))parent=static_cast<std::uint32_t>(j);
        loop.parent=parent;
    }
    for(std::size_t i=0;i<result.loops.size();++i) {
        auto& loop=result.loops[i];loop.depth=1;for(auto parent=loop.parent;parent;parent=result.loops[*parent].parent)++loop.depth;
        if(!loop.available)continue;
        for(const auto block:loop.blocks) {
            const auto prior=result.innermost_of_block[block];
            if(prior<0 || loop.blocks.size()<result.loops[static_cast<std::size_t>(prior)].blocks.size())result.innermost_of_block[block]=static_cast<std::int32_t>(i);
        }
    }
    return result;
}
} // namespace quidra::ir::analysis
