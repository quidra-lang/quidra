#pragma once
#include "ir/facts/entry.hpp"
#include "quidra/ir/module.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quidra::ir::analysis { class AnalysisCache; }
namespace quidra::optimizer {
struct SpecializationOptions {
    bool exact_binding{true};
    std::size_t maximum_clones{4}, small_instruction_limit{400};
    std::uint64_t maximum_operations{2000000};
    std::size_t maximum_rounds{32};
};
struct CloneOrigin { std::string clone, source; bool unchanged_body{}; };
struct SpecializationUnavailable { std::string function, reason; };
struct HelperSpecializationReport {
    std::size_t rewritten_calls{}, generated_clones{}, folded_tracking{}, updated_entries{};
    std::uint64_t operations{}, budget_fallbacks{};
    ir::facts::EntryFactsMap entries;
    std::vector<std::string> removed_entries;
    std::vector<CloneOrigin> clones;
    std::vector<SpecializationUnavailable> unavailable;
    std::shared_ptr<const ir::analysis::BindingOriginSnapshots> origin_owner;
    bool changed() const noexcept { return rewritten_calls || folded_tracking || updated_entries || !removed_entries.empty(); }
};
class HelperSpecialization {
public:
    // Collects all facts before mutation and appends only after call rewrites.
    // The caller publishes report.entries into Function::validated_entry, refreshes
    // summaries/S1 source mappings with clones, and invalidates analyses.
    static HelperSpecializationReport run(ir::Module&, ir::analysis::AnalysisCache&,
        const ir::facts::EntryFactsMap& prior_entries = {}, SpecializationOptions = {});
};
} // namespace quidra::optimizer
