#include "ir/analysis/call_graph.hpp"

#include "quidra/ir/module.hpp"

#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::ir::analysis {

std::unordered_set<std::string> node_cleanup_functions(const ir::Module& module) {
    std::unordered_map<std::string, const ir::Function*> callable;
    std::unordered_set<std::string> address_taken;
    std::vector<std::string> pending;
    for (const auto& function : module.functions) {
        if (!function.entrypoint) callable[function.name] = &function;
        if (function.parallel_node) {
            address_taken.insert(function.name);
            pending.push_back(function.name);
        }
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                if (const auto* ref = std::get_if<ir::FunctionRef>(&instruction))
                    address_taken.insert(ref->function);
                if (const auto* parallel = std::get_if<ir::ParallelBlock>(&instruction))
                    for (const auto& node : parallel->nodes) pending.push_back(node.function);
            }
    }
    std::unordered_set<std::string> result;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto name = pending[index];
        if (!result.insert(name).second) continue;
        const auto found = callable.find(name);
        if (found == callable.end() || found->second->external_symbol) continue;
        for (const auto& block : found->second->blocks)
            for (const auto& instruction : block.instructions) {
                bool callback = std::holds_alternative<ir::IndirectCall>(instruction);
                if (const auto* call = std::get_if<ir::Call>(&instruction)) {
                    const auto callee = callable.find(call->callee);
                    if (callee != callable.end()) {
                        if (callee->second->external_symbol) callback = true;
                        else pending.push_back(call->callee);
                    }
                }
                if (callback) {
                    for (const auto& target : address_taken) pending.push_back(target);
                    for (const auto& target : module.functions)
                        if (target.c_export_symbol) pending.push_back(target.name);
                }
            }
    }
    return result;
}

std::unordered_set<std::string> recursive_functions(const ir::Module& module) {
    std::unordered_set<std::string> names;
    for (const auto& function : module.functions) names.insert(function.name);

    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const auto& function : module.functions) {
        auto& edges = graph[function.name];
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* call = std::get_if<ir::Call>(&instruction);
                    call && names.contains(call->callee)) {
                    edges.push_back(call->callee);
                }
                if(const auto* parallel=std::get_if<ir::ParallelBlock>(&instruction))
                    for(const auto& node:parallel->nodes)edges.push_back(node.function);
            }
        }
    }

    std::unordered_set<std::string> result;
    for (const auto& function : module.functions) {
        const auto& start = function.name;
        std::unordered_set<std::string> visited;
        std::vector<std::string> pending;
        visited.insert(start);
        pending.push_back(start);
        bool reaches_start = false;
        while(!pending.empty() && !reaches_start) {
            auto current = std::move(pending.back());
            pending.pop_back();
            const auto it = graph.find(current);
            if(it == graph.end()) continue;
            for(const auto& next : it->second) {
                if(next == start) {
                    reaches_start = true;
                    break;
                }
                if(visited.insert(next).second) pending.push_back(next);
            }
        }
        if(reaches_start) result.insert(start);
    }
    return result;
}

std::unordered_set<std::string> self_depth_recursive_functions(
    const ir::Module& module,
    const std::unordered_set<std::string>& recursive,
    const std::unordered_set<std::string>& address_taken) {
    std::unordered_map<std::string,std::vector<std::string>> graph;
    for(const auto& function:module.functions) {
        auto& edges=graph[function.name];
        for(const auto& block:function.blocks)
            for(const auto& instruction:block.instructions) {
                if(const auto* call=std::get_if<ir::Call>(&instruction))edges.push_back(call->callee);
                if(const auto* parallel=std::get_if<ir::ParallelBlock>(&instruction))
                    for(const auto& node:parallel->nodes)edges.push_back(node.function);
            }
    }

    const auto reaches=[&](const std::string& start,const std::string& target) {
        std::unordered_set<std::string> visited;
        std::vector<std::string> pending;
        visited.insert(start);
        pending.push_back(start);
        while(!pending.empty()) {
            auto current=std::move(pending.back());
            pending.pop_back();
            const auto found=graph.find(current);
            if(found==graph.end()) continue;
            for(const auto& next:found->second) {
                if(next==target) return true;
                if(visited.insert(next).second) pending.push_back(next);
            }
        }
        return false;
    };

    std::unordered_set<std::string> result;
    for(const auto& function:module.functions) {
        if(!recursive.contains(function.name)||
           address_taken.contains(function.name))
            continue;

        bool direct_self=false;
        bool indirect_call=false;
        for(const auto& block:function.blocks) {
            for(const auto& instruction:block.instructions) {
                if(const auto* call=std::get_if<ir::Call>(&instruction);
                   call&&call->callee==function.name)
                    direct_self=true;
                if(std::holds_alternative<ir::IndirectCall>(instruction))
                    indirect_call=true;
            }
        }
        if(!direct_self||indirect_call) continue;

        bool shares_recursive_depth=false;
        for(const auto& other:recursive) {
            if(other==function.name) continue;
            if(reaches(function.name,other)||reaches(other,function.name)) {
                shares_recursive_depth=true;
                break;
            }
        }
        if(!shares_recursive_depth) result.insert(function.name);
    }
    return result;
}

} // namespace quidra::ir::analysis
