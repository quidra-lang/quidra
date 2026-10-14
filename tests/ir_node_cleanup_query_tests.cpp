#include "ir/analysis/call_graph.hpp"
#include "quidra/ir/module.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <utility>

using namespace quidra;
using namespace quidra::ir;
namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
Function function(std::string name, std::vector<Instruction> body) {
    Function result; result.name = std::move(name);
    result.blocks.push_back({"entry", std::move(body)});
    return result;
}
}
int main() {
    const auto nothing = Type::simple(TypeKind::Void);
    ParallelBlock parallel; parallel.nodes.push_back({"worker", {}, {}, {}, false, false});
    auto entry = function("entry", {parallel, ReturnVoid{}});
    entry.entrypoint = true;
    auto worker = function("worker", {Call{0, "managed", {}, nothing}, FunctionRef{1, "callback", Type::function(nothing)}, IndirectCall{0, 1, {}, {}, nothing}, ReturnVoid{}});
    auto metadata_root = function("metadata_root", {ReturnVoid{}});
    metadata_root.parallel_node = ParallelNodeMetadata{"wrapper", {}, false};
    auto managed = function("managed", {Call{0, "host", {}, nothing}, Call{0, "missing", {}, nothing}, ReturnVoid{}});
    auto host = function("host", {}); host.external_symbol = "host_abi";
    auto callback = function("callback", {Call{0, "leaf", {}, nothing}, ReturnVoid{}});
    auto leaf = function("leaf", {ReturnVoid{}});
    auto exported = function("exported", {ReturnVoid{}}); exported.c_export_symbol = "public_abi";
    Module module; module.functions = {entry, worker, metadata_root, managed, host, callback, leaf, exported, function("unreachable", {ReturnVoid{}})};
    const std::unordered_set<std::string> expected{"worker", "metadata_root", "managed", "callback", "leaf", "exported"};
    const auto query = analysis::node_cleanup_functions(module, std::numeric_limits<std::uint64_t>::max());
    require(query.available && !query.budget_exhausted && query.functions == expected,
            "typed roots, direct calls, indirect/external callbacks and C exports share backend reachability");
    require(analysis::node_cleanup_functions(module) == expected,
            "the existing unbounded backend query preserves the exact same semantics");
    // Nine functions + 15 discovery instructions + 12 reachable instructions,
    // ten pending insertions + ten pending attempts + six callback expansions.
    require(query.operations == 62, "all actual traversal work is counted");
    const auto exact = analysis::node_cleanup_functions(module, query.operations);
    require(exact.available && exact.functions == expected && exact.operations == query.operations,
            "the exact remaining budget admits a complete authoritative result");
    const auto short_query = analysis::node_cleanup_functions(module, query.operations - 1);
    require(!short_query.available && short_query.budget_exhausted && short_query.functions.empty() && short_query.operations == query.operations - 1,
            "exhaustion never publishes a partial or successfully empty cleanup map");
    const auto zero = analysis::node_cleanup_functions(module, 0);
    require(!zero.available && zero.budget_exhausted && zero.operations == 0 && zero.functions.empty(),
            "zero budget never scans a function or grants an empty-map fallback");
}
