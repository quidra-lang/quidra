#include "ir/analysis/dominators.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace quidra::ir::analysis {
namespace {

struct Graph {
    std::vector<std::vector<BlockId>> succ, pred;
};

void validate(const Cfg& cfg) {
    const auto count = cfg.succ.size();
    if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::length_error("IR dominator graph has too many blocks");
    if (cfg.pred.size() != count || cfg.reachable.size() != count || cfg.rpo_index.size() != count)
        throw std::invalid_argument("IR dominator CFG dimensions disagree");
    for (const auto& edges : cfg.succ)
        for (const auto target : edges)
            if (target >= count) throw std::invalid_argument("IR dominator CFG edge is out of range");
    for (const auto* exits : {&cfg.normal_exits, &cfg.abnormal_exits})
        for (const auto exit : *exits)
            if (exit >= count) throw std::invalid_argument("IR dominator CFG exit is out of range");
}

void predecessors(Graph& graph) {
    graph.pred.resize(graph.succ.size());
    for (std::size_t block = 0; block < graph.succ.size(); ++block)
        for (const auto target : graph.succ[block])
            graph.pred[target].push_back(static_cast<BlockId>(block));
}

std::vector<BlockId> reverse_postorder(const Graph& graph, BlockId root) {
    std::vector<bool> seen(graph.succ.size());
    std::vector<std::pair<BlockId, std::size_t>> pending{{root, 0}};
    std::vector<BlockId> postorder;
    seen[root] = true;
    while (!pending.empty()) {
        auto& [block, next] = pending.back();
        if (next == graph.succ[block].size()) {
            postorder.push_back(block); pending.pop_back();
        } else {
            const auto target = graph.succ[block][next++];
            if (!seen[target]) { seen[target] = true; pending.emplace_back(target, 0); }
        }
    }
    return {postorder.rbegin(), postorder.rend()};
}

BlockId intersect(BlockId first, BlockId second, const DomTree& tree,
                  const std::vector<std::uint32_t>& order) {
    while (first != second) {
        while (order[first] > order[second]) first = static_cast<BlockId>(tree.idom[first]);
        while (order[second] > order[first]) second = static_cast<BlockId>(tree.idom[second]);
    }
    return first;
}

void number_tree(DomTree& tree) {
    if (!tree.root) return;
    // Separate preorder and finish counters each visit at most one entry per
    // node, so adding virtual exit does not double the uint32 timestamp range.
    std::uint32_t pre = 0, post = 0;
    std::vector<std::pair<BlockId, std::size_t>> pending{{*tree.root, 0}};
    tree.pre[*tree.root] = pre++;
    while (!pending.empty()) {
        auto& [block, next] = pending.back();
        if (next == tree.children[block].size()) {
            tree.post[block] = post++; pending.pop_back();
        } else {
            const auto child = tree.children[block][next++];
            tree.pre[child] = pre++; pending.emplace_back(child, 0);
        }
    }
}

DomTree build(const Graph& graph, BlockId root, BlockId original_count, DomScope scope,
              bool abnormal) {
    const auto count = graph.succ.size();
    DomTree tree;
    tree.block_count = original_count; tree.scope = scope; tree.includes_abnormal_exits = abnormal;
    tree.idom.assign(count, -1); tree.children.resize(count); tree.frontier.resize(count);
    tree.pre.assign(count, kNoRpo); tree.post.assign(count, kNoRpo);
    if (!count) return tree;
    tree.root = root;
    const auto rpo = reverse_postorder(graph, root);
    std::vector<std::uint32_t> order(count, kNoRpo);
    for (std::size_t i = 0; i < rpo.size(); ++i) order[rpo[i]] = static_cast<std::uint32_t>(i);
    tree.idom[root] = static_cast<std::int32_t>(root);
    bool changed;
    do {
        changed = false;
        for (std::size_t i = 1; i < rpo.size(); ++i) {
            const auto block = rpo[i];
            std::int32_t parent = -1;
            for (const auto predecessor : graph.pred[block]) {
                if (tree.idom[predecessor] < 0) continue;
                parent = parent < 0 ? static_cast<std::int32_t>(predecessor) :
                    static_cast<std::int32_t>(intersect(static_cast<BlockId>(parent), predecessor, tree, order));
            }
            if (tree.idom[block] != parent) { tree.idom[block] = parent; changed = true; }
        }
    } while (changed);
    for (std::size_t block = 0; block < count; ++block) {
        if (tree.idom[block] < 0) continue;
        const auto parent = static_cast<BlockId>(tree.idom[block]);
        if (block != root) tree.children[parent].push_back(static_cast<BlockId>(block));
        // All reachable incoming edges matter, including an entry backedge:
        // entry has no strict dominator despite idom[entry]=entry.
        for (const auto predecessor : graph.pred[block]) {
            if (tree.idom[predecessor] < 0) continue;
            auto runner = predecessor;
            while (runner != parent) {
                auto& frontier = tree.frontier[runner];
                // Targets are visited in authored order. A previous walk
                // for this target already covered the same ancestor suffix.
                if (!frontier.empty() && frontier.back() == block) break;
                frontier.push_back(static_cast<BlockId>(block));
                runner = static_cast<BlockId>(tree.idom[runner]);
            }
            if (runner == block) {
                auto& frontier = tree.frontier[runner];
                if (frontier.empty() || frontier.back() != block)
                    frontier.push_back(static_cast<BlockId>(block));
            }
        }
    }
    for (auto& frontier : tree.frontier) {
        std::sort(frontier.begin(), frontier.end());
        frontier.erase(std::unique(frontier.begin(), frontier.end()), frontier.end());
    }
    number_tree(tree);
    return tree;
}

} // namespace

bool DomTree::dominates(BlockId ancestor, BlockId descendant) const noexcept {
    if (ancestor >= pre.size() || descendant >= pre.size() ||
        ancestor >= post.size() || descendant >= post.size() ||
        pre[ancestor] == kNoRpo || pre[descendant] == kNoRpo ||
        post[ancestor] == kNoRpo || post[descendant] == kNoRpo) return false;
    return pre[ancestor] <= pre[descendant] && post[descendant] <= post[ancestor];
}

bool DomTree::dominates(InstrRef ancestor, InstrRef descendant) const noexcept {
    if (ancestor.block >= block_count || descendant.block >= block_count ||
        !dominates(ancestor.block, descendant.block)) return false;
    if (ancestor.block != descendant.block) return true;
    return scope == DomScope::FiniteSelectedExitPaths ? ancestor.index >= descendant.index :
        ancestor.index <= descendant.index;
}

DomTree dominators(const Cfg& cfg) {
    validate(cfg);
    Graph graph{cfg.succ, {}};
    predecessors(graph);
    return build(graph, 0, static_cast<BlockId>(cfg.succ.size()), DomScope::EntryPaths, false);
}

DomTree post_dominators(const Cfg& cfg, bool abnormal_exits_reach_virtual_exit) {
    validate(cfg);
    const auto count = static_cast<BlockId>(cfg.succ.size());
    Graph graph;
    if (count) {
        graph.succ.resize(static_cast<std::size_t>(count) + 1);
        for (BlockId block = 0; block < count; ++block)
            if (cfg.reachable[block]) for (const auto target : cfg.succ[block])
                graph.succ[target].push_back(block);
        std::vector<bool> selected(count);
        for (const auto exit : cfg.normal_exits) selected[exit] = true;
        if (abnormal_exits_reach_virtual_exit)
            for (const auto exit : cfg.abnormal_exits) selected[exit] = true;
        for (BlockId block = 0; block < count; ++block)
            if (selected[block] && cfg.reachable[block]) graph.succ[count].push_back(block);
        predecessors(graph);
    }
    return build(graph, count, count, DomScope::FiniteSelectedExitPaths, abnormal_exits_reach_virtual_exit);
}

} // namespace quidra::ir::analysis
