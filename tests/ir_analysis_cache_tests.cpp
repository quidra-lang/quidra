#include "ir/analysis/analysis_cache.hpp"
#include "ir/facts/entry.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void cache_reuse_and_invalidation() {
    Module module;
    Function function;
    function.blocks = {{"entry", {ConstantBool{1, true}, Return{1, Type::simple(TypeKind::Bool)}}}};
    module.functions = {function, function};
    AnalysisCache cache(module);
    require(cache.evaluations(AnalysisCache::Kind::Cfg) == 0, "cache eagerly analyzes an unused function");
    const auto* first = &cache.liveness(0);
    require(&cache.liveness(0) == first && cache.evaluations(AnalysisCache::Kind::Liveness) == 1,
            "repeated analysis is not reused");
    require(cache.evaluations(AnalysisCache::Kind::Cfg) == 1 && cache.evaluations(AnalysisCache::Kind::Aliases) == 1,
            "dependencies are computed more than once");
    const auto* second = &cache.cfg(1);
    module.functions[0].blocks = {{"entry", {Jump{"done"}}}, {"done", {ReturnVoid{}}}};
    cache.invalidate(0);
    require(cache.cfg(0).succ.size() == 2, "invalidated analysis retains old blocks");
    require(&cache.cfg(1) == second && cache.evaluations(AnalysisCache::Kind::Cfg) == 3,
            "per-function invalidation discards unrelated analyses");
    const auto* loops = &cache.loops(0);
    require(&cache.loops(0) == loops && cache.evaluations(AnalysisCache::Kind::Loops) == 1,
            "loop topology is not lazily reused");
    require(cache.loop_invariants(0).loops.empty() &&
            cache.evaluations(AnalysisCache::Kind::LoopInvariants) == 1,
            "straight-line function gains a fabricated invariant loop");
    require(cache.liveness(0).blocks.size() == 2 && cache.evaluations(AnalysisCache::Kind::Liveness) == 2,
            "dependent analysis is stale after invalidation");
    const auto& with_error = cache.post_dominators(0, true);
    const auto& normal = cache.post_dominators(0, false);
    require(with_error.includes_abnormal_exits && !normal.includes_abnormal_exits &&
            cache.evaluations(AnalysisCache::Kind::PostDominators) == 2, "postdominator options share the wrong cache key");
    cache.invalidate_all();
    (void)cache.cfg(1);
    require(cache.evaluations(AnalysisCache::Kind::Cfg) == 4, "module invalidation retains stale entries");
    bool rejected = false;
    try { (void)cache.cfg(99); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "unknown function accepted");
}
void validated_entry_invalidation() {
    Module module;
    Function fn;
    fn.name = "helper";
    fn.declaration = DeclarationIdentity{};
    fn.declaration->source_name = "helper";
    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32));
    fn.parameters = {{"x", tensor}};
    fn.blocks = {{"entry", {LoadLocal{1, "x", tensor}, Return{1, tensor}}}};
    module.functions.push_back(std::move(fn));
    AnalysisCache cache(module);
    require(cache.tensor_facts(0).at({0,1},1).type.length == -1,
            "unchecked signature acquires a rank proof");
    auto entry = std::make_shared<facts::ValidatedEntryFacts>();
    entry->declaration = *module.functions[0].declaration;
    auto& fact = entry->tensors[0];
    fact.has_type = true;
    fact.type = Type::tensor(Type::simple(TypeKind::Real32), 2);
    fact.extents = {ShapeExtent(4), ShapeExtent(5)};
    fact.minimum_rank = 2;
    fact.rank_extent = ShapeExtent(2);
    fact.tracked = facts::Tri::False;
    // Even malformed owner bits must not become a caller proof of ownership.
    fact.unique = facts::Tri::True;
    fact.owns_storage = true;
    fact.fully_initialized = facts::Tri::True;
    module.functions[0].validated_entry = entry;
    cache.invalidate(0);
    const auto validated = cache.tensor_facts(0).at({0,1},1);
    require(validated.type.length == 2 && validated.tracked == facts::Tri::False &&
            validated.extents.size() == 2 && validated.extents[0].known() == 4,
            "owned entry contract did not reach the actual parameter load");
    require(validated.unique == facts::Tri::Unknown && !validated.owns_storage &&
            validated.fully_initialized == facts::Tri::Unknown,
            "caller ownership or initialization leaked into the callee");
    module.functions[0].validated_entry.reset();
    cache.invalidate(0);
    require(cache.tensor_facts(0).at({0,1},1).type.length == -1,
            "withdrawn all-call-site entry contract remains cached");
}
} // namespace
int main() {
    try { cache_reuse_and_invalidation(); validated_entry_invalidation(); std::cout << "IR analysis cache tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
