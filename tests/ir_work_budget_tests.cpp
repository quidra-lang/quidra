#include "ir/analysis/analysis_cache.hpp"
#include "ir/analysis/work_budget.hpp"
#include "ir/verify.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void exhausted(F action, WorkBudgetReason reason) {
    try {
        action();
    } catch (const WorkBudgetExhausted& error) {
        require(error.reason == reason, "wrong work-budget fallback reason");
        return;
    }
    throw std::runtime_error("work beyond the request ceiling succeeded");
}
Module input() {
    Module module;
    Function fn;
    fn.name = "budgeted";
    fn.result = Type::simple(TypeKind::Int64);
    const auto integer = fn.result;
    fn.blocks = {
        {"entry", {ConstantBool{1, true}, Branch{1, "left", "right"}}},
        {"left", {ConstantInt{2, "7", integer}, Return{2, integer}}},
        {"right", {ConstantInt{3, "9", integer}, Clone{4, 3, integer}, Return{4, integer}}}};
    module.functions.push_back(std::move(fn));
    return module;
}
void limits() {
    WorkBudget budget(
        {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::size_t>::max(), 1});
    budget.take(std::numeric_limits<std::uint64_t>::max());
    budget.take(0);
    exhausted([&] { budget.take(); }, WorkBudgetReason::Operations);
    require(budget.used_operations() == std::numeric_limits<std::uint64_t>::max(),
            "failed charge wrapped operation count");
    WorkBudget bytes({100, std::numeric_limits<std::size_t>::max(), 1});
    bytes.bytes(std::numeric_limits<std::size_t>::max());
    exhausted([&] { bytes.bytes(1); }, WorkBudgetReason::Bytes);
    require(bytes.remaining_bytes() == 0, "failed byte charge changed the ceiling");
    WorkBudget depth({100, 100, 1});
    {
        WorkBudget::DepthGuard guard(depth);
        exhausted([&] { WorkBudget::DepthGuard nested(depth); }, WorkBudgetReason::Depth);
    }
    require(depth.current_depth() == 0, "failed depth entry leaked recursion depth");
}
void real_cfg_and_cache() {
    const auto module = input();
    WorkBudget zero({0, 1000, 256});
    AnalysisCache empty(module, &zero);
    exhausted([&] { (void)empty.cfg(0); }, WorkBudgetReason::Operations);
    require(empty.evaluations(AnalysisCache::Kind::Cfg) == 0,
            "budget failure published a partial CFG");
    WorkBudget short_labels({1000, 0, 256});
    exhausted([&] { (void)build_cfg(module.functions[0], short_labels); }, WorkBudgetReason::Bytes);
    WorkBudget budget({10000, 10000, 256});
    AnalysisCache cache(module, &budget);
    const auto& graph = cache.cfg(0);
    const auto legacy = build_cfg(module.functions[0]);
    require(graph.succ == legacy.succ && graph.pred == legacy.pred && graph.rpo == legacy.rpo &&
                graph.reachable == legacy.reachable,
            "metering changed authored CFG semantics");
    const auto& checked = cache.verify(0);
    const auto original = ir::verify(module.functions[0], &module);
    require(checked.ssa_valid == original.ssa_valid &&
                checked.dominance_complete == original.dominance_complete &&
                checked.roles_complete == original.roles_complete &&
                checked.diagnostics == original.diagnostics &&
                checked.unverified_dominance == original.unverified_dominance &&
                checked.unresolved_roles == original.unresolved_roles,
            "cached verification changed validity or availability");
    require(cache.evaluations(AnalysisCache::Kind::Cfg) == 1 &&
                cache.evaluations(AnalysisCache::Kind::Dominators) == 1 &&
                cache.evaluations(AnalysisCache::Kind::DefUse) == 1,
            "verification recomputed its actual upstream inputs");
    require(cache.unmetered(AnalysisCache::Kind::Dominators) &&
                cache.unmetered(AnalysisCache::Kind::DefUse),
            "uncounted dependencies were hidden");
    require(!cache.upstream_budget_complete(),
            "partial producer metering claimed an end-to-end budget");
    budget.take(budget.remaining_operations());
    const auto used = budget.used_operations();
    require(&cache.cfg(0) == &graph && &cache.verify(0) == &checked &&
                budget.used_operations() == used,
            "zero-ceiling cache hit rebuilt or charged producer work");
    cache.invalidate(0);
    exhausted([&] { (void)cache.cfg(0); }, WorkBudgetReason::Operations);
    require(cache.evaluations(AnalysisCache::Kind::Cfg) == 1,
            "failed rebuild replaced a previously valid CFG");
}
} // namespace
int main() {
    try {
        limits();
        real_cfg_and_cache();
        std::puts("shared analysis budget: essential checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
