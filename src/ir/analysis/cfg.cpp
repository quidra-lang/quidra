#include "ir/analysis/cfg.hpp"
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
    if (std::holds_alternative<Return>(instruction) || std::holds_alternative<ReturnVoid>(instruction))
        return End::Normal;
    if (std::holds_alternative<Exit>(instruction) || std::holds_alternative<InlineExit>(instruction) || std::holds_alternative<FailError>(instruction))
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
    void successor(std::string_view label) override { labels.push_back(label); }
    std::vector<std::string_view> labels;
};

} // namespace

Cfg build_cfg(const Function& function) {
    // Keep block IDs representable by the signed immediate-dominator index
    // used by the subsequent analysis, including its virtual exit.
    if (function.blocks.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::length_error("IR CFG has too many blocks");
    const auto count = function.blocks.size();
    Cfg cfg;
    cfg.succ.resize(count); cfg.pred.resize(count);
    cfg.reachable.assign(count, false); cfg.rpo_index.assign(count, kNoRpo);
    cfg.label.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& block = function.blocks[i];
        if (!cfg.label.emplace(block.label, static_cast<BlockId>(i)).second)
            throw std::invalid_argument("IR CFG duplicate block label '" + block.label + "'");
        if (block.instructions.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("IR CFG block has too many instructions");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& block = function.blocks[i];
        const auto id = static_cast<BlockId>(i);
        for (std::size_t j = 0; j + 1 < block.instructions.size(); ++j)
            if (ending(block.instructions[j]) != End::Missing)
                throw std::invalid_argument("IR CFG instruction follows terminator in '" + block.label + "'");
        const auto end = block.instructions.empty() ? End::Missing : ending(block.instructions.back());
        if (end == End::Normal) cfg.normal_exits.push_back(id);
        else if (end == End::Missing || end == End::Abnormal) cfg.abnormal_exits.push_back(id);
        else {
            Successors visitor;
            visit_operands(block.instructions.back(), visitor);
            for (const auto name : visitor.labels) {
                const auto target = cfg.label.find(std::string(name));
                if (target == cfg.label.end())
                    throw std::invalid_argument("IR CFG missing target '" + std::string(name) + "' from '" + block.label + "'");
                auto& edges = cfg.succ[i];
                if (std::find(edges.begin(), edges.end(), target->second) == edges.end()) {
                    edges.push_back(target->second);
                    cfg.pred[target->second].push_back(id);
                }
            }
        }
    }
    if (!count) return cfg;
    // Explicit DFS frames avoid consuming native stack for deep graphs.
    std::vector<std::pair<BlockId, std::size_t>> pending{{0, 0}};
    std::vector<BlockId> postorder;
    cfg.reachable[0] = true;
    while (!pending.empty()) {
        auto& [block, next] = pending.back();
        if (next == cfg.succ[block].size()) {
            postorder.push_back(block); pending.pop_back();
        } else {
            const auto successor = cfg.succ[block][next++];
            if (!cfg.reachable[successor]) {
                cfg.reachable[successor] = true;
                pending.emplace_back(successor, 0);
            }
        }
    }
    cfg.rpo.assign(postorder.rbegin(), postorder.rend());
    for (std::size_t i = 0; i < cfg.rpo.size(); ++i)
        cfg.rpo_index[cfg.rpo[i]] = static_cast<std::uint32_t>(i);
    return cfg;
}

} // namespace quidra::ir::analysis
