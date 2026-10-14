#pragma once

#include "quidra/ir/module.hpp"
#include "semantics/effect_summary.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string_view>

namespace quidra::ir::analysis {
enum class ResultAlias : std::uint8_t { Fresh, ParamDerived, Unknown };
namespace effect_bit {
inline constexpr std::uint32_t HostSync = 1U << 0, DeviceWork = 1U << 1, Alloc = 1U << 2,
    MayFail = 1U << 3, MayFailDeferred = 1U << 4, ReadsPolicy = 1U << 5,
    WritesPolicy = 1U << 6, ExternalIO = 1U << 7, Opaque = 1U << 8;
}
struct SemanticBinding {
    std::string semantic_key;
    // Exact lowering provenance. Synthetic parameters have no semantic index.
    std::vector<std::optional<std::size_t>> semantic_parameter_for_ir_parameter;
};
using SummaryBindings = std::map<std::string, SemanticBinding, std::less<>>;

// Separate typed IR proofs; semantic Fresh is not TensorStorage freshness.
// Missing facts are unavailable, never a pure or allocation-free contract.
struct IrSummaryFacts {
    ResultAlias result{ResultAlias::Unknown};
    std::optional<std::size_t> result_parameter;
    bool result_complete{};
    bool descriptor_effects{}, descriptor_complete{};
    std::uint32_t effects{effect_bit::Opaque};
    bool effects_complete{};
};
using IrSummaryInputs = std::map<std::string, IrSummaryFacts, std::less<>>;
struct FunctionSummary : IrSummaryFacts {
    const semantics::EffectSummary* semantic{}; // Owned by ModuleSummaries.
    std::string semantic_key;
    std::vector<std::optional<std::size_t>> semantic_parameter_for_ir_parameter;
    bool pure_tensor{};
};
struct ModuleSummaries {
    using SemanticSnapshots = std::map<std::string, semantics::EffectSummary, std::less<>>;
    ModuleSummaries() = default;
    ModuleSummaries(const ModuleSummaries&) = delete;
    ModuleSummaries& operator=(const ModuleSummaries&) = delete;
    ModuleSummaries(ModuleSummaries&&) = default;
    ModuleSummaries& operator=(ModuleSummaries&&) = delete;
    std::map<std::string, FunctionSummary, std::less<>> by_function;
    const FunctionSummary* find(std::string_view name) const noexcept;
    const SemanticSnapshots& semantic_snapshots() const noexcept;
private:
    // Shared immutable imported values; enrichment never recopies semantic
    // rows or retains AST-keyed checker maps.
    std::shared_ptr<const SemanticSnapshots> semantic_;
    friend struct CloneContractAccess;
    friend ModuleSummaries adapt_summaries(const Module&, const semantics::EffectSummaries&,
                                           const SummaryBindings&);
    friend ModuleSummaries apply_ir_summary_facts(const Module&, const ModuleSummaries&,
                                                 const IrSummaryInputs&);
};
ModuleSummaries adapt_summaries(const Module&, const semantics::EffectSummaries&,
                               const SummaryBindings&);
// Import is independent of CFG/call/alias analysis. The optimizer enriches
// its records from supplied typed facts while sharing the semantic snapshot.
ModuleSummaries apply_ir_summary_facts(const Module&, const ModuleSummaries&, const IrSummaryInputs&);
} // namespace quidra::ir::analysis
