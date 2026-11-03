#pragma once
#include "ir/analysis/def_use.hpp"
#include <cstdint>
#include <optional>
#include <vector>
namespace quidra::ir::analysis {
struct Cfg;
struct DomTree;
class WorkBudget;
} // namespace quidra::ir::analysis
namespace quidra::ir {
enum class SsaIssue : std::uint8_t {
    ZeroValue,
    MissingDefinition,
    MultipleDefinitions,
    DefinitionNotEarlier,
    DefinitionDoesNotDominate
};
struct SsaDiagnostic {
    SsaIssue issue{};
    ValueId value{};
    analysis::InstrRef at;
    std::optional<analysis::InstrRef> definition;
    std::uint32_t ordinal{};
    bool operator==(const SsaDiagnostic&) const = default;
};
struct OperandSite {
    ValueId value{};
    analysis::Use use;
    bool operator==(const OperandSite&) const = default;
};
// SSA validity excludes no-global-def/duplicate/zero/strict order/reachable
// dominance violations. Separate completeness flags prevent unreachable
// cross-block or unresolved ownership roles from becoming a proof.
struct VerifyReport {
    bool ssa_valid{true}, dominance_complete{true}, roles_complete{true};
    std::vector<SsaDiagnostic> diagnostics;
    std::vector<OperandSite> unverified_dominance, unresolved_roles;
};
// Observational only: no consumer/pass integration. CFG structural errors
// propagate from build_cfg; all indices refer to the unchanged function.
VerifyReport verify(const Function&, const Module* module = nullptr);
// Inputs must be the actual analyses of this unchanged function. Cache clients
// use this overload to avoid recomputing CFG/dominance/operand occurrences.
VerifyReport verify(const Function&, const analysis::Cfg&, const analysis::DomTree&,
                    const analysis::DefUse&, analysis::WorkBudget* budget = nullptr);
} // namespace quidra::ir
