// RegionCandidateAnnotation (region_candidates.hpp). Both annotation lists
// are sorted and deduplicated, so they do not depend on hash order.
#include "optimizer/region_candidates.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace quidra::optimizer {

using ir::Call;
using ir::Module;
using ir::ValueId;

namespace {

struct RegionOperationOccurrence {
    std::string reference;
    ValueId output{};
    std::vector<ValueId> inputs;
};

} // namespace

bool RegionCandidateAnnotation::run(Module& module, const PassContext& context) {
    const auto& extension_tables = context.registry.extension_tables().tables;
    const auto& extension_fusions = context.registry.extension_tables().fusions;
    const auto& call_operations = context.registry.bindings().call_operations;
    for (auto& function : module.functions) {
        for (auto& region : function.tensor_regions) {
            region.compiler_extension_tables.clear();
            for (const auto& identity : region.compiler_extensions) {
                const auto found = extension_tables.find(identity);
                if (found == extension_tables.end()) continue;
                for (const auto& table : found->second) {
                    region.compiler_extension_tables.push_back(
                        identity + ":" + table);
                }
            }
            std::sort(
                region.compiler_extension_tables.begin(),
                region.compiler_extension_tables.end());
            region.compiler_extension_tables.erase(
                std::unique(
                    region.compiler_extension_tables.begin(),
                    region.compiler_extension_tables.end()),
                region.compiler_extension_tables.end());

            std::vector<RegionOperationOccurrence> occurrences;
            for (const auto& location : region.instructions) {
                if (location.block >= function.blocks.size() ||
                    location.instruction >=
                        function.blocks[location.block].instructions.size()) {
                    continue;
                }
                const auto* call = std::get_if<Call>(
                    &function.blocks[location.block]
                         .instructions[location.instruction]);
                if (!call) continue;
                const auto found = call_operations.find(call->callee);
                if (found == call_operations.end()) continue;

                std::vector<ValueId> inputs;
                for (const auto& argument : call->args) {
                    if (argument.value != 0)
                        inputs.push_back(argument.value);
                }
                for (const auto& operation : found->second) {
                    occurrences.push_back(
                        RegionOperationOccurrence{
                            operation, call->out, inputs});
                }
            }

            region.compiler_fusion_candidates.clear();
            for (const auto& [identity, patterns] : extension_fusions) {
                const auto prefix = identity + ":";
                for (const auto& pattern : patterns) {
                    if (pattern.operations.size() < 2) continue;

                    std::vector<std::size_t> frontier;
                    for (std::size_t index = 0;
                         index < occurrences.size(); ++index) {
                        if (occurrences[index].reference ==
                            pattern.operations.front()) {
                            frontier.push_back(index);
                        }
                    }

                    for (std::size_t position = 1;
                         position < pattern.operations.size() &&
                         !frontier.empty();
                         ++position) {
                        std::vector<std::size_t> next;
                        for (const auto current : frontier) {
                            for (std::size_t candidate = 0;
                                 candidate < occurrences.size();
                                 ++candidate) {
                                if (!occurrences[candidate].reference.starts_with(
                                        prefix) ||
                                    occurrences[candidate].reference !=
                                        pattern.operations[position] ||
                                    std::find(
                                        occurrences[candidate].inputs.begin(),
                                        occurrences[candidate].inputs.end(),
                                        occurrences[current].output) ==
                                        occurrences[candidate].inputs.end()) {
                                    continue;
                                }
                                next.push_back(candidate);
                            }
                        }
                        std::sort(next.begin(), next.end());
                        next.erase(
                            std::unique(next.begin(), next.end()),
                            next.end());
                        frontier = std::move(next);
                    }

                    if (!frontier.empty())
                        region.compiler_fusion_candidates.push_back(
                            pattern.reference);
                }
            }
            std::sort(
                region.compiler_fusion_candidates.begin(),
                region.compiler_fusion_candidates.end());
            region.compiler_fusion_candidates.erase(
                std::unique(
                    region.compiler_fusion_candidates.begin(),
                    region.compiler_fusion_candidates.end()),
                region.compiler_fusion_candidates.end());
        }
    }
    return false;
}

} // namespace quidra::optimizer
