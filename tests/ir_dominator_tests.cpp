#include <iostream>

#include "ir/analysis/dominators.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace allocation_traffic {
bool measuring{};
std::size_t bytes{};
void count(std::size_t size) noexcept {
    if (!measuring) return;
    const auto remaining = std::numeric_limits<std::size_t>::max() - bytes;
    bytes += std::min(size, remaining);
}
struct Measurement {
    Measurement() { bytes = 0; measuring = true; }
    ~Measurement() { measuring = false; }
};
}
void* operator new(std::size_t size) {
    if (void* value = std::malloc(size ? size : 1)) {
        allocation_traffic::count(size); return value;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Function graph(std::vector<Block> blocks) {
    Function function;
    function.blocks = std::move(blocks);
    return function;
}

using Table = std::vector<std::vector<bool>>;
struct Oracle { Table relation; std::vector<bool> available; };

// Enumerate simple paths independently of the CHK/RPO/tree algorithm. A
// finite path avoiding a candidate has a simple path with that property.
Oracle entry_paths(const Cfg& cfg) {
    const auto count = cfg.succ.size();
    Oracle result{Table(count, std::vector<bool>(count)), std::vector<bool>(count)};
    if (!count) return result;
    const auto all = static_cast<std::uint16_t>((1U << count) - 1);
    std::vector<std::uint16_t> common(count, all);
    const auto visit = [&](const auto& self, BlockId block, std::uint16_t path) -> void {
        path = static_cast<std::uint16_t>(path | (1U << block));
        result.available[block] = true;
        common[block] = static_cast<std::uint16_t>(common[block] & path);
        for (const auto next : cfg.succ[block])
            if (!(path & (1U << next))) self(self, next, path);
    };
    visit(visit, 0, 0);
    for (std::size_t block = 0; block < count; ++block)
        for (std::size_t candidate = 0; candidate < count; ++candidate)
            result.relation[candidate][block] = result.available[block] &&
                (common[block] & (1U << candidate));
    return result;
}

Oracle selected_exit_paths(const Cfg& cfg, bool abnormal) {
    const auto count = cfg.succ.size();
    if (!count) return {};
    const auto entry = entry_paths(cfg);
    Oracle result{Table(count + 1, std::vector<bool>(count + 1)),
                  std::vector<bool>(count + 1)};
    std::vector<bool> exit(count);
    for (const auto block : cfg.normal_exits) exit[block] = true;
    if (abnormal) for (const auto block : cfg.abnormal_exits) exit[block] = true;
    const auto all = static_cast<std::uint16_t>((1U << count) - 1);
    for (std::size_t start = 0; start < count; ++start) {
        if (!entry.available[start]) continue;
        auto common = all;
        const auto visit = [&](const auto& self, BlockId block, std::uint16_t path) -> void {
            path = static_cast<std::uint16_t>(path | (1U << block));
            if (exit[block]) {
                result.available[start] = true;
                common = static_cast<std::uint16_t>(common & path);
            }
            for (const auto next : cfg.succ[block])
                if (!(path & (1U << next))) self(self, next, path);
        };
        visit(visit, static_cast<BlockId>(start), 0);
        if (!result.available[start]) continue;
        result.relation[count][start] = true;
        for (std::size_t candidate = 0; candidate < count; ++candidate)
            result.relation[candidate][start] = (common & (1U << candidate)) != 0;
    }
    result.available[count] = true;
    result.relation[count][count] = true;
    return result;
}

void compare(const DomTree& tree, const Oracle& oracle,
             const std::vector<std::vector<BlockId>>& successors) {
    const auto count = oracle.available.size();
    require(tree.idom.size() == count && tree.frontier.size() == count &&
                tree.children.size() == count && tree.pre.size() == count && tree.post.size() == count,
            "dominance arrays differ from graph domain");
    for (std::size_t block = 0; block < count; ++block) {
        if (!oracle.available[block]) {
            require(tree.idom[block] == -1 && tree.pre[block] == kNoRpo && tree.post[block] == kNoRpo,
                    "unavailable node acquired a tree relation");
        } else {
            std::int32_t parent = -1;
            if (block == tree.root) parent = static_cast<std::int32_t>(block);
            else for (std::size_t candidate = 0; candidate < count; ++candidate) {
                if (candidate == block || !oracle.relation[candidate][block]) continue;
                bool immediate = true;
                for (std::size_t ancestor = 0; ancestor < count; ++ancestor)
                    if (ancestor != block && ancestor != candidate && oracle.relation[ancestor][block] &&
                            !oracle.relation[ancestor][candidate]) immediate = false;
                if (immediate) parent = static_cast<std::int32_t>(candidate);
            }
            require(tree.idom[block] == parent, "immediate dominator differs from path oracle");
        }
        for (std::size_t candidate = 0; candidate < count; ++candidate)
            require(tree.dominates(static_cast<BlockId>(candidate), static_cast<BlockId>(block)) ==
                        oracle.relation[candidate][block], "dominance differs from path oracle");
    }
    for (std::size_t candidate = 0; candidate < count; ++candidate) {
        std::vector<BlockId> expected, children;
        for (std::size_t block = 0; block < count; ++block) {
            if (tree.idom[block] == static_cast<std::int32_t>(candidate) && block != candidate)
                children.push_back(static_cast<BlockId>(block));
            bool frontier = false;
            for (std::size_t predecessor = 0; predecessor < count; ++predecessor)
                if (oracle.relation[candidate][predecessor] && oracle.available[block] &&
                        std::find(successors[predecessor].begin(), successors[predecessor].end(), block) !=
                            successors[predecessor].end() &&
                        (candidate == block || !oracle.relation[candidate][block])) frontier = true;
            if (frontier) expected.push_back(static_cast<BlockId>(block));
        }
        require(tree.frontier[candidate] == expected, "dominance frontier differs from edge oracle");
        require(tree.children[candidate] == children, "dominator children are not in authored order");
    }
}

std::vector<std::vector<BlockId>> reverse_graph(const Cfg& cfg, bool abnormal) {
    const auto count = cfg.succ.size();
    if (!count) return {};
    std::vector<std::vector<BlockId>> successors(count + 1);
    for (std::size_t block = 0; block < count; ++block)
        if (cfg.reachable[block]) for (const auto next : cfg.succ[block])
            successors[next].push_back(static_cast<BlockId>(block));
    for (const auto block : cfg.normal_exits)
        if (cfg.reachable[block]) successors[count].push_back(block);
    if (abnormal) for (const auto block : cfg.abnormal_exits)
        if (cfg.reachable[block]) successors[count].push_back(block);
    return successors;
}

void verify(const Function& function) {
    const auto cfg = build_cfg(function);
    compare(dominators(cfg), entry_paths(cfg), cfg.succ);
    for (const bool abnormal : {false, true})
        compare(post_dominators(cfg, abnormal), selected_exit_paths(cfg, abnormal),
                reverse_graph(cfg, abnormal));
}

void explicit_graphs() {
    const auto diamond = graph({{"entry", {Branch{1, "left", "right"}}},
        {"left", {ConstantBool{2, false}, Jump{"join"}}}, {"right", {Jump{"join"}}}, {"join", {ReturnVoid{}}},
        {"dead", {FailError{2, {1, 1}}}}});
    verify(diamond);
    const auto cfg = build_cfg(diamond);
    const auto dom = dominators(cfg), pdom = post_dominators(cfg);
    require(dom.idom == std::vector<std::int32_t>({0, 0, 0, 0, -1}), "diamond immediate dominators");
    require(pdom.idom == std::vector<std::int32_t>({3, 3, 3, 5, -1, 5}), "diamond postdominators");
    require(dom.scope == DomScope::EntryPaths && pdom.scope == DomScope::FiniteSelectedExitPaths,
            "relation scope is not explicit");
    require(dom.dominates(InstrRef{1, 0}, InstrRef{1, 1}) &&
                !dom.dominates(InstrRef{1, 1}, InstrRef{1, 0}), "forward instruction ordering");
    require(pdom.dominates(InstrRef{1, 1}, InstrRef{1, 0}) &&
                !pdom.dominates(InstrRef{1, 0}, InstrRef{1, 1}), "reverse instruction ordering");
    require(!pdom.dominates(InstrRef{5, 0}, InstrRef{1, 0}) &&
                !dom.dominates(InstrRef{4, 0}, InstrRef{4, 0}) &&
                !dom.dominates(99, 0), "undefined or virtual instruction relation");
    const auto again = post_dominators(cfg);
    require(again.idom == pdom.idom && again.children == pdom.children && again.frontier == pdom.frontier &&
                again.pre == pdom.pre && again.post == pdom.post, "tree traversal is nondeterministic");

    verify(graph({{"entry", {Branch{1, "body", "exit"}}},
        {"body", {Jump{"entry"}}}, {"exit", {ReturnVoid{}}}}));
    const auto entry_loop = dominators(build_cfg(graph({{"entry", {Jump{"body"}}},
        {"body", {Jump{"entry"}}}})));
    require(entry_loop.frontier[0] == std::vector<BlockId>{0}, "entry backedge frontier omitted");
    verify(graph({{"entry", {Jump{"outer"}}}, {"outer", {Branch{1, "inner", "exit"}}},
        {"inner", {Branch{2, "body", "next"}}}, {"body", {Jump{"inner"}}},
        {"next", {Jump{"outer"}}}, {"exit", {ReturnVoid{}}}}));
    verify(graph({{"entry", {Branch{1, "a", "b"}}}, {"a", {Branch{2, "b", "exit"}}},
        {"b", {Branch{3, "a", "exit"}}}, {"exit", {ReturnVoid{}}}}));

    const auto mixed = build_cfg(graph({{"entry", {Branch{1, "ok", "error"}}},
        {"ok", {ReturnVoid{}}}, {"error", {FailError{2, {1, 1}}}}}));
    require(post_dominators(mixed).includes_abnormal_exits &&
                !post_dominators(mixed, false).includes_abnormal_exits &&
                post_dominators(mixed).idom[0] == 3 && post_dominators(mixed, false).idom[0] == 1 &&
                post_dominators(mixed, false).idom[2] == -1, "abnormal exit selection");
    verify(graph({{"entry", {Branch{1, "ok", "error"}}},
        {"ok", {ReturnVoid{}}}, {"error", {FailError{2, {1, 1}}}}}));
    verify(graph({{"entry", {Branch{1, "missing", "never"}}}, {"missing", {}},
        {"never", {Call{0, "stop", {}, Type::simple(TypeKind::Never)}}}}));
    const auto infinite = build_cfg(graph({{"entry", {Branch{1, "exit", "loop"}}},
        {"exit", {ReturnVoid{}}}, {"loop", {Jump{"loop"}}}, {"dead", {ReturnVoid{}}}}));
    const auto finite = post_dominators(infinite);
    require(finite.idom == std::vector<std::int32_t>({1, 4, -1, -1, 4}) && finite.dominates(1, 0),
            "finite selected-exit relation invented a loop exit or excluded its finite path");
    verify(graph({{"entry", {Branch{1, "exit", "loop"}}},
        {"exit", {ReturnVoid{}}}, {"loop", {Jump{"loop"}}}, {"dead", {ReturnVoid{}}}}));
    verify(graph({{"entry", {Jump{"cycle"}}}, {"cycle", {Jump{"entry"}}}}));
    const auto empty = build_cfg(graph({}));
    require(dominators(empty).idom.empty() && !dominators(empty).root &&
                post_dominators(empty).idom.empty() && !post_dominators(empty).root, "empty graph has a root");
}

struct Random {
    std::uint32_t state;
    std::uint32_t next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
};
void random_paths() {
    for (std::uint32_t seed = 1; seed <= 10000; ++seed) {
        Random random{seed ^ 0x9e3779b9U};
        const auto count = 1U + random.next() % 12;
        Function function;
        for (std::uint32_t block = 0; block < count; ++block) {
            Block out{"b" + std::to_string(block), {}};
            const auto kind = random.next() % 6;
            const auto target = [&] { return "b" + std::to_string(random.next() % count); };
            if (kind == 0) out.instructions.push_back(ReturnVoid{});
            else if (kind == 1) out.instructions.push_back(FailError{1, {1, 1}});
            else if (kind == 2) out.instructions.push_back(Jump{target()});
            else out.instructions.push_back(Branch{1, target(), target()});
            function.blocks.push_back(std::move(out));
        }
        try { verify(function); }
        catch (const std::exception& error) {
            throw std::runtime_error("CFG seed " + std::to_string(seed) + ": " + error.what());
        }
    }
}

void deep_graph() {
    Function function;
    constexpr std::uint32_t count = 25000;
    function.blocks.reserve(count);
    for (std::uint32_t block = 0; block < count; ++block) {
        Block out{"b" + std::to_string(block), {}};
        if (block + 1 == count) out.instructions.push_back(ReturnVoid{});
        else out.instructions.push_back(Jump{"b" + std::to_string(block + 1)});
        function.blocks.push_back(std::move(out));
    }
    const auto cfg = build_cfg(function);
    const auto dom = dominators(cfg), pdom = post_dominators(cfg);
    require(dom.dominates(0, count - 1) && dom.idom[count - 1] == count - 2 &&
                pdom.dominates(count - 1, 0) && pdom.idom[0] == 1,
            "deep iterative tree or query failed");
}

void sparse_common_exit() {
    Function function;
    constexpr BlockId count = 1024;
    for (BlockId block = 0; block < count; ++block) {
        Block out{"b" + std::to_string(block), {}};
        if (block + 1 == count) out.instructions.push_back(ReturnVoid{});
        else out.instructions.push_back(Branch{1, "b" + std::to_string(block + 1),
                                                "b" + std::to_string(count - 1)});
        function.blocks.push_back(std::move(out));
    }
    const auto cfg = build_cfg(function);
    DomTree tree;
    std::size_t bytes = 0;
    {
        const allocation_traffic::Measurement measuring;
        tree = dominators(cfg);
        bytes = allocation_traffic::bytes;
    }
    require(tree.frontier[0].empty() && tree.frontier[count - 1].empty(), "common-exit root frontier");
    for (BlockId block = 1; block + 1 < count; ++block)
        require(tree.frontier[block] == std::vector<BlockId>{count - 1}, "common-exit unique frontier");
    // A sparse graph with a linear result must not construct a quadratic
    // transient frontier. This generous byte bound counts allocation traffic,
    // independent of elapsed time and standard-library growth details.
    require(bytes <= std::size_t(count) * 1024, "sparse common-exit dominance used superlinear temporary storage");
}
} // namespace

int main() {
    try {
        explicit_graphs(); random_paths(); deep_graph(); sparse_common_exit();
        std::cout << "IR dominance: selected-exit scope and 10000 path oracles verified\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
