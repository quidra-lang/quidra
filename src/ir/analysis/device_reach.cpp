#include "ir/analysis/device_reach.hpp"

#include "quidra/ir/module.hpp"

#include <unordered_map>
#include <variant>
#include <vector>

namespace quidra::ir::analysis {
namespace {

// Whether the instruction itself may leave device work behind.
bool reaches_device(const ir::Instruction& instruction) {
    return std::visit(
        [](const auto& node) {
            using Node = std::decay_t<decltype(node)>;
            constexpr auto domain = InstructionTraits<Node>::domain;
            return domain == Domain::tensor || domain == Domain::autograd ||
                   std::is_same_v<Node, TaskAll>;
        },
        instruction);
}

} // namespace

std::unordered_set<std::string> device_reaching_functions(const ir::Module& module) {
    std::unordered_map<std::string, const ir::Function*> functions;
    std::unordered_set<std::string> address_taken;
    for (const auto& function : module.functions) {
        functions.emplace(function.name, &function);
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (const auto* ref = std::get_if<ir::FunctionRef>(&instruction))
                    address_taken.insert(ref->function);
    }

    // The callers of each function, and the functions that reach device
    // work by themselves.
    std::unordered_map<std::string, std::vector<std::string>> callers;
    std::vector<std::string> pending;
    std::unordered_set<std::string> result;
    const auto mark = [&](const std::string& name) {
        if (result.insert(name).second) pending.push_back(name);
    };
    for (const auto& function : module.functions) {
        if (function.external_symbol) {
            mark(function.name);
            continue;
        }
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* call = std::get_if<ir::Call>(&instruction)) {
                    callers[call->callee].push_back(function.name);
                    // A callee the module does not define is not Quidra code.
                    if (!functions.contains(call->callee)) mark(function.name);
                } else if (std::holds_alternative<ir::IndirectCall>(instruction)) {
                    for (const auto& target : address_taken)
                        callers[target].push_back(function.name);
                } else if (reaches_device(instruction)) {
                    mark(function.name);
                }
            }
        }
    }

    // Everything that may call a function in the result is in the result.
    while (!pending.empty()) {
        const auto name = std::move(pending.back());
        pending.pop_back();
        const auto found = callers.find(name);
        if (found == callers.end()) continue;
        for (const auto& caller : found->second) mark(caller);
    }
    return result;
}

} // namespace quidra::ir::analysis
