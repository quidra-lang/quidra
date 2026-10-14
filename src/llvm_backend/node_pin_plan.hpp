#pragma once
#include "quidra/ir/module.hpp"
#include "llvm_backend/inline_scope_plan.hpp"
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace quidra::llvm_backend {
// Match releases by control-flow provenance, rather than emitter visitation
// order. Both arms of a branch may release the same acquired pin. Balanced
// loop backedges preserve their entry obligations and converge by set union.
struct NodePinPlan {
    std::map<const ir::Instruction*,std::string> acquisitions;
    std::vector<std::string> acquisition_order;
    std::map<std::string,std::uint64_t> scopes;
    std::map<const ir::Instruction*,std::set<std::string>> releases;

    explicit NodePinPlan(const ir::Function& function):NodePinPlan(function,InlineScopePlan(function)){}
    NodePinPlan(const ir::Function& function,const InlineScopePlan& ownership,bool root_cleanup=true) {
        using Candidates=std::set<std::string>;
        using Stack=std::vector<Candidates>;
        using State=std::map<ir::ValueId,Stack>;
        std::map<std::string,std::size_t> labels;
        for(std::size_t i=0;i<function.blocks.size();++i) {
            labels.emplace(function.blocks[i].label,i);
            for(const auto& instruction:function.blocks[i].instructions)
                if(std::holds_alternative<ir::Pin>(instruction) && ownership.enabled(ownership.instruction_scope(instruction),root_cleanup)) {
                    auto key="%quidra.pin."+std::to_string(acquisition_order.size());
                    scopes.emplace(key,ownership.instruction_scope(instruction));
                    acquisitions.emplace(&instruction,key);acquisition_order.push_back(std::move(key));
                }
        }
        if(function.blocks.empty())return;
        std::vector<State> entries(function.blocks.size());
        std::vector<bool> reached(function.blocks.size()),queued(function.blocks.size());
        std::deque<std::size_t> work{0};reached[0]=queued[0]=true;
        const auto propagate=[&](const std::string& label,const State& state) {
            const auto next=labels.at(label);bool changed=false;
            if(!reached[next]) {entries[next]=state;reached[next]=true;changed=true;}
            else {
                auto& destination=entries[next];
                // A join with different pin counts cannot safely guess which
                // runtime acquisition an explicit Unpin is consuming.
                if(destination.size()!=state.size())
                    throw std::logic_error("parallel pin obligations differ at a control-flow join");
                for(const auto& [target,stack]:state) {
                    const auto found=destination.find(target);
                    if(found==destination.end() || found->second.size()!=stack.size())
                        throw std::logic_error("parallel pin depth differs at a control-flow join");
                    for(std::size_t depth=0;depth<stack.size();++depth) {
                        auto& candidates=found->second[depth];const auto before=candidates.size();
                        candidates.insert(stack[depth].begin(),stack[depth].end());
                        changed=changed || candidates.size()!=before;
                    }
                }
            }
            if(changed && !queued[next]){work.push_back(next);queued[next]=true;}
        };
        while(!work.empty()) {
            const auto index=work.front();work.pop_front();queued[index]=false;
            auto state=entries[index];
            for(const auto& instruction:function.blocks[index].instructions) {
                if(const auto* pin=std::get_if<ir::Pin>(&instruction)) {
                    if(!acquisitions.contains(&instruction))continue;
                    auto& stack=state[pin->value];
                    if(stack.size()>=acquisitions.size())
                        throw std::logic_error("parallel pin obligations accumulate across a loop");
                    stack.push_back(Candidates{acquisitions.at(&instruction)});
                } else if(const auto* unpin=std::get_if<ir::Unpin>(&instruction)) {
                    if(!ownership.enabled(ownership.instruction_scope(instruction),root_cleanup))continue;
                    const auto found=state.find(unpin->value);
                    if(found==state.end() || found->second.empty())
                        throw std::logic_error("parallel unpin lacks a reaching acquisition");
                    auto& candidates=releases[&instruction];
                    candidates.insert(found->second.back().begin(),found->second.back().end());
                    found->second.pop_back();if(found->second.empty())state.erase(found);
                } else if(const auto* cleanup=std::get_if<ir::InlineScopeCleanup>(&instruction)) {
                    for(auto at=state.begin();at!=state.end();) {
                        auto& stack=at->second;
                        std::erase_if(stack,[&](const auto& candidates) {
                            bool own=false,foreign=false;
                            for(const auto& key:candidates) {if(scopes.at(key)==cleanup->scope)own=true;else foreign=true;}
                            if(own && foreign)throw std::logic_error("pin cleanup merges different inline scope owners");
                            return own;
                        });
                        if(stack.empty())at=state.erase(at);else ++at;
                    }
                } else if(const auto* jump=std::get_if<ir::Jump>(&instruction)) {
                    propagate(jump->target,state);break;
                } else if(const auto* branch=std::get_if<ir::Branch>(&instruction)) {
                    propagate(branch->if_true,state);propagate(branch->if_false,state);break;
                } else if(std::holds_alternative<ir::Return>(instruction) ||
                          std::holds_alternative<ir::ReturnVoid>(instruction) ||
                          std::holds_alternative<ir::Exit>(instruction) ||
                          std::holds_alternative<ir::InlineExit>(instruction) ||
                          std::holds_alternative<ir::FailError>(instruction))break;
                else if(const auto* call=std::get_if<ir::Call>(&instruction)) {
                    if(call->result.kind==TypeKind::Never || call->no_normal_return)break;
                } else if(const auto* call=std::get_if<ir::IndirectCall>(&instruction)) {
                    if(call->result.kind==TypeKind::Never)break;
                }
            }
        }
    }
};
}
