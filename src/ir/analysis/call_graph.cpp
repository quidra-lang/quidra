#include "ir/analysis/call_graph.hpp"

#include "quidra/ir/module.hpp"

#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::ir::analysis {

namespace {
NodeCleanupReachability cleanup_reachability(
    const ir::Module& module, std::optional<std::uint64_t> maximum_operations) {
    NodeCleanupReachability result;
    const auto charge = [&]() {
        if (!maximum_operations) return true;
        if (result.operations == *maximum_operations) {
            result.budget_exhausted = true;
            return false;
        }
        ++result.operations;
        return true;
    };
    const auto unavailable = [&]() {
        result.functions.clear();
        return std::move(result);
    };
    std::unordered_map<std::string, const ir::Function*> callable;
    std::unordered_set<std::string> address_taken;
    std::vector<std::string> exported;
    std::vector<std::string> pending;
    const auto enqueue = [&](const std::string& name) {
        if (!charge()) return false;
        pending.push_back(name);
        return true;
    };
    for (const auto& function : module.functions) {
        if (!charge()) return unavailable();
        if (!function.entrypoint) callable[function.name] = &function;
        if (function.c_export_symbol) exported.push_back(function.name);
        if (function.parallel_node) {
            address_taken.insert(function.name);
            if (!enqueue(function.name)) return unavailable();
        }
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                if (!charge()) return unavailable();
                if (const auto* ref = std::get_if<ir::FunctionRef>(&instruction))
                    address_taken.insert(ref->function);
                if (const auto* parallel = std::get_if<ir::ParallelBlock>(&instruction))
                    for (const auto& node : parallel->nodes)
                        if (!enqueue(node.function)) return unavailable();
            }
    }
    for (std::size_t index = 0; index < pending.size(); ++index) {
        if (!charge()) return unavailable();
        const auto name = pending[index];
        if (!result.functions.insert(name).second) continue;
        const auto found = callable.find(name);
        if (found == callable.end() || found->second->external_symbol) continue;
        for (const auto& block : found->second->blocks)
            for (const auto& instruction : block.instructions) {
                if (!charge()) return unavailable();
                bool callback = std::holds_alternative<ir::IndirectCall>(instruction);
                if (const auto* call = std::get_if<ir::Call>(&instruction)) {
                    const auto callee = callable.find(call->callee);
                    if (callee != callable.end()) {
                        if (callee->second->external_symbol) callback = true;
                        else if (!enqueue(call->callee)) return unavailable();
                    }
                }
                if (callback) {
                    for (const auto& target : address_taken) {
                        if (!charge() || !enqueue(target)) return unavailable();
                    }
                    for (const auto& target : exported) {
                        if (!charge() || !enqueue(target)) return unavailable();
                    }
                }
            }
    }
    result.available = true;
    return result;
}
} // namespace

std::unordered_set<std::string> node_cleanup_functions(const ir::Module& module) {
    return cleanup_reachability(module, std::nullopt).functions;
}

NodeCleanupReachability node_cleanup_functions(
    const ir::Module& module, std::uint64_t maximum_operations) {
    return cleanup_reachability(module, maximum_operations);
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
