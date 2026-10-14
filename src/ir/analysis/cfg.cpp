#include "ir/analysis/cfg.hpp"
#include "ir/analysis/work_budget.hpp"
#include "quidra/ir/visit.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace quidra::ir::analysis {
namespace {

enum class End { Missing, Successors, Normal, Abnormal };
End ending(const Instruction& instruction) {
    if (std::holds_alternative<Jump>(instruction) || std::holds_alternative<Branch>(instruction))
        return End::Successors;
    if (std::holds_alternative<Return>(instruction) ||
        std::holds_alternative<ReturnVoid>(instruction))
        return End::Normal;
    if (std::holds_alternative<Exit>(instruction) ||
        std::holds_alternative<InlineExit>(instruction) ||
        std::holds_alternative<FailError>(instruction))
        return End::Abnormal;
    if (const auto* call = std::get_if<Call>(&instruction);
        call && (call->no_normal_return || call->result.kind == TypeKind::Never))
        return End::Abnormal;
    if (const auto* call = std::get_if<IndirectCall>(&instruction);
        call && call->result.kind == TypeKind::Never)
        return End::Abnormal;
    return End::Missing;
}

struct Successors final : OperandVisitor {
    explicit Successors(WorkBudget* value) : budget(value) {}
    void successor(std::string_view label) override {
        if (budget)
            budget->take();
        labels.push_back(label);
    }
    WorkBudget* budget;
    std::vector<std::string_view> labels;
};

} // namespace

namespace {
Cfg build_cfg_impl(const Function& function, WorkBudget* budget) {
    const auto take = [&](std::uint64_t count = 1) {
        if (budget)
            budget->take(count);
    };
    const auto bytes = [&](std::size_t count) {
        if (budget)
            budget->bytes(count);
    };
    // Keep block IDs representable by the signed immediate-dominator index
    // used by the subsequent analysis, including its virtual exit.
    if (function.blocks.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::length_error("IR CFG has too many blocks");
    const auto count = function.blocks.size();
    // Charge bulk output slots before allocating any graph containers.
    take(static_cast<std::uint64_t>(count) * 6);
    // Bound label copying/hashing before graph or name-map allocation.
    for (const auto& block : function.blocks) {
        take();
        bytes(block.label.size());
        take(block.label.size());
    }
    Cfg cfg;
    cfg.succ.resize(count);
    cfg.pred.resize(count);
    cfg.reachable.assign(count, false);
    cfg.rpo_index.assign(count, kNoRpo);
    cfg.label.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        take();
        const auto& block = function.blocks[i];
        if (!cfg.label.emplace(block.label, static_cast<BlockId>(i)).second)
            throw std::invalid_argument("IR CFG duplicate block label '" + block.label + "'");
        if (block.instructions.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("IR CFG block has too many instructions");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& block = function.blocks[i];
        take();
        const auto id = static_cast<BlockId>(i);
        for (std::size_t j = 0; j + 1 < block.instructions.size(); ++j) {
            take();
            if (ending(block.instructions[j]) != End::Missing)
                throw std::invalid_argument("IR CFG instruction follows terminator in '" +
                                            block.label + "'");
        }
        take();
        const auto end =
            block.instructions.empty() ? End::Missing : ending(block.instructions.back());
        if (end == End::Normal)
            cfg.normal_exits.push_back(id);
        else if (end == End::Missing || end == End::Abnormal)
            cfg.abnormal_exits.push_back(id);
        else {
            Successors visitor(budget);
            visit_operands(block.instructions.back(), visitor);
            for (const auto name : visitor.labels) {
                bytes(name.size());
                take(name.size());
                const auto target = cfg.label.find(std::string(name));
                if (target == cfg.label.end())
                    throw std::invalid_argument("IR CFG missing target '" + std::string(name) +
                                                "' from '" + block.label + "'");
                auto& edges = cfg.succ[i];
                bool present = false;
                for (const auto edge : edges) {
                    take();
                    if (edge == target->second) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    edges.push_back(target->second);
                    cfg.pred[target->second].push_back(id);
                }
            }
        }
    }
    if (!count)
        return cfg;
    // Explicit DFS frames avoid consuming native stack for deep graphs.
    std::vector<std::pair<BlockId, std::size_t>> pending{{0, 0}};
    std::vector<BlockId> postorder;
    cfg.reachable[0] = true;
    while (!pending.empty()) {
        take();
        auto& [block, next] = pending.back();
        if (next == cfg.succ[block].size()) {
            postorder.push_back(block);
            pending.pop_back();
        } else {
            const auto successor = cfg.succ[block][next++];
            if (!cfg.reachable[successor]) {
                cfg.reachable[successor] = true;
                pending.emplace_back(successor, 0);
            }
        }
    }
    take(postorder.size());
    cfg.rpo.assign(postorder.rbegin(), postorder.rend());
    for (std::size_t i = 0; i < cfg.rpo.size(); ++i) {
        take();
        cfg.rpo_index[cfg.rpo[i]] = static_cast<std::uint32_t>(i);
    }
    return cfg;
}

} // namespace
Cfg build_cfg(const Function& function) {
    return build_cfg_impl(function, nullptr);
}
Cfg build_cfg(const Function& function, WorkBudget& budget) {
    return build_cfg_impl(function, &budget);
}

} // namespace quidra::ir::analysis
