#include "ir/analysis/cfg.hpp"
#include "ir/analysis/call_graph.hpp"
#include "quidra/ir/module.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, message);
}

void diamond_and_explicit_error_edges() {
    Function function;
    function.blocks = {
        {"entry", {SourceLocation{{1, 1}, {}, {}, {}, {}}, Branch{1, "success", "error"}}},
        {"success", {Jump{"join"}}},
        {"error", {Jump{"join"}}},
        {"join", {ReturnVoid{}}}};
    const auto cfg = build_cfg(function);
    require(cfg.succ == std::vector<std::vector<BlockId>>{{1, 2}, {3}, {3}, {}}, "diamond successors");
    require(cfg.pred == std::vector<std::vector<BlockId>>{{}, {0}, {0}, {1, 2}}, "diamond predecessors");
    require(cfg.rpo == std::vector<BlockId>{0, 2, 1, 3} &&
                cfg.rpo_index == std::vector<std::uint32_t>{0, 2, 1, 3}, "deterministic reverse postorder");
    require(cfg.reachable == std::vector<bool>{true, true, true, true}, "diamond reachability");
    require(cfg.normal_exits == std::vector<BlockId>{3} && cfg.abnormal_exits.empty(), "normal return classification");
    function.blocks[2].instructions = {FailError{2}};
    const auto failed = build_cfg(function);
    require(failed.succ[2].empty() && failed.abnormal_exits == std::vector<BlockId>{2} &&
                failed.pred[3] == std::vector<BlockId>{1}, "explicit failure branch remains an exit");
    const auto repeated = build_cfg(function);
    require(repeated.rpo == failed.rpo && repeated.pred == failed.pred && repeated.label == failed.label,
            "repeated construction is deterministic");
}

void loops_unreachable_and_duplicate_edges() {
    Function function;
    function.blocks = {{"entry", {Jump{"cond"}}},
        {"cond", {Branch{1, "body", "done"}}}, {"body", {Jump{"cond"}}},
        {"done", {ReturnVoid{}}}, {"dead", {Jump{"dead"}}}};
    const auto cfg = build_cfg(function);
    require(cfg.pred[1] == std::vector<BlockId>{0, 2} && cfg.pred[4] == std::vector<BlockId>{4},
            "loop back edges and unreachable cycle retained");
    require(cfg.reachable == std::vector<bool>{true, true, true, true, false} && cfg.rpo_index[4] == kNoRpo,
            "unreachable blocks have no RPO index");
    require(cfg.rpo == std::vector<BlockId>{0, 1, 3, 2}, "loop reverse postorder");
    Function repeated;
    repeated.blocks = {{"entry", {Branch{1, "done", "done"}}}, {"done", {ReturnVoid{}}}};
    const auto same = build_cfg(repeated);
    require(same.succ[0] == std::vector<BlockId>{1} && same.pred[1] == std::vector<BlockId>{0},
            "repeated branch destinations are one graph edge");
}

void nested_loops() {
    Function function;
    function.blocks = {{"entry", {Jump{"outer"}}},
        {"outer", {Branch{1, "inner", "done"}}},
        {"inner", {Branch{2, "work", "advance"}}},
        {"work", {Jump{"inner"}}}, {"advance", {Jump{"outer"}}},
        {"dead", {ReturnVoid{}}}, {"done", {ReturnVoid{}}}};
    const auto cfg = build_cfg(function);
    require(cfg.pred[1] == std::vector<BlockId>{0, 4} && cfg.pred[2] == std::vector<BlockId>{1, 3},
            "nested loops retain both independent back edges");
    require(cfg.rpo == std::vector<BlockId>{0, 1, 6, 2, 4, 3} && !cfg.reachable[5],
            "nested loops have deterministic reachable traversal");
}

void exits_and_missing_terminators() {
    const auto never = Type::simple(TypeKind::Never);
    Function function;
    function.blocks = {{"entry", {Branch{1, "ok", "failure"}}}, {"ok", {ReturnVoid{}}},
        {"failure", {FailError{2}}}, {"exit", {Exit{3}}},
        {"direct", {Call{0, "stop", {}, Type::simple(TypeKind::Void), {0, 0}, true}}},
        {"never", {Call{0, "stop", {}, never}}},
        {"indirect", {IndirectCall{0, 1, {}, {}, never}}}, {"empty", {}},
        {"missing", {ConstantBool{4, true}}},
        {"return", {Call{4, "ordinary", {}, Type::simple(TypeKind::Bool)}, Return{4, Type::simple(TypeKind::Bool)}}}};
    const auto cfg = build_cfg(function);
    require(cfg.normal_exits == std::vector<BlockId>{1, 9} &&
                cfg.abnormal_exits == std::vector<BlockId>{2, 3, 4, 5, 6, 7, 8}, "all exit kinds classified");
    require(cfg.succ[7].empty() && cfg.succ[8].empty() && !cfg.reachable[9],
            "unterminated blocks never fall through lexically");
    const auto empty = build_cfg(Function{});
    require(empty.succ.empty() && empty.rpo.empty() && empty.normal_exits.empty() && empty.abnormal_exits.empty(),
            "empty declaration has an empty graph");
}

void malformed_graphs_and_deep_chain() {
    Function function;
    function.blocks = {{"entry", {Jump{"absent"}}}};
    rejects([&] { (void)build_cfg(function); }, "missing target accepted");
    function.blocks = {{"same", {ReturnVoid{}}}, {"same", {ReturnVoid{}}}};
    rejects([&] { (void)build_cfg(function); }, "duplicate labels accepted");
    function.blocks = {{"entry", {ReturnVoid{}, ConstantBool{1, true}}}};
    rejects([&] { (void)build_cfg(function); }, "instructions after a terminator accepted");
    constexpr BlockId count = 25000;
    function.blocks.clear(); function.blocks.reserve(count);
    for (BlockId i = 0; i < count; ++i) {
        Block block{std::to_string(i), {}};
        if (i + 1 < count) block.instructions.push_back(Jump{std::to_string(i + 1)});
        else block.instructions.push_back(ReturnVoid{});
        function.blocks.push_back(std::move(block));
    }
    const auto deep = build_cfg(function);
    require(deep.rpo.size() == count && deep.rpo.front() == 0 && deep.rpo.back() == count - 1 &&
                deep.normal_exits == std::vector<BlockId>{count - 1}, "deep CFG uses bounded iterative traversal");
}
void typed_parallel_cleanup_modes() {
    Module module;
    Function root, worker, leaf, foreign, callback, exported, decoy;
    root.name = "main"; root.entrypoint = true;
    worker.name = "worker";
    worker.parallel_node = ParallelNodeMetadata{"worker.entry", {}, false};
    worker.blocks = {{"entry", {Call{0,"leaf",{},Type::simple(TypeKind::Void)},
        Call{0,"foreign",{},Type::simple(TypeKind::Void)},ReturnVoid{}}}};
    leaf.name = "leaf"; leaf.blocks = {{"entry", {ReturnVoid{}}}};
    foreign.name = "foreign"; foreign.external_symbol = "foreign_callback";
    callback.name = "callback"; callback.blocks = {{"entry", {ReturnVoid{}}}};
    exported.name = "exported"; exported.c_export_symbol = "exported";
    exported.blocks = {{"entry", {ReturnVoid{}}}};
    decoy.name = "$par.unrelated"; decoy.blocks = {{"entry", {ReturnVoid{}}}};
    root.blocks = {{"entry", {FunctionRef{1,"callback",Type::simple(TypeKind::Function)}, ReturnVoid{}}}};
    module.functions = {root,worker,leaf,foreign,callback,exported,decoy};
    const auto modes = node_cleanup_functions(module);
    require(modes.contains("worker") && modes.contains("leaf") && modes.contains("callback") && modes.contains("exported"),
            "typed node/call/callback cleanup reachability was lost");
    require(!modes.contains("main") && !modes.contains("$par.unrelated"),
            "generated spelling incorrectly authorizes a cleanup mode");
    Function exit;
    exit.blocks = {{"entry", {InlineExit{1,2}}}};
    require(build_cfg(exit).abnormal_exits == std::vector<BlockId>{0},
            "inlined process exit is not a terminal abnormal edge");
}
} // namespace

int main() {
    try {
        diamond_and_explicit_error_edges();
        loops_unreachable_and_duplicate_edges();
        nested_loops();
        exits_and_missing_terminators();
        malformed_graphs_and_deep_chain();
        typed_parallel_cleanup_modes();
        std::cout << "IR CFG: explicit edges, exits, reachability and deterministic order verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
