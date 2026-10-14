#include "ir/verify.hpp"
#include "ir/analysis/dominators.hpp"

namespace quidra::ir {
VerifyReport verify(const Function& function, const Module* module) {
    const auto cfg = analysis::build_cfg(function);
    const auto dom = analysis::dominators(cfg);
    const auto chain = analysis::def_use(function, module);
    VerifyReport result;
    const auto invalid = [&](SsaIssue issue, ValueId value, analysis::InstrRef at,
                             std::optional<analysis::InstrRef> definition = {},
                             std::uint32_t ordinal = 0) {
        result.ssa_valid = false;
        result.diagnostics.push_back({issue, value, at, definition, ordinal});
    };
    // Numeric ID order is stable; occurrence order within each ID is authored.
    for (const auto& record : chain.values) {
        for (const auto& use : record.uses) {
            if (!use.role_available) {
                result.roles_complete = false;
                result.unresolved_roles.push_back({record.value, use});
            }
        }
        if (record.value == 0) {
            for (const auto at : record.definitions) invalid(SsaIssue::ZeroValue, 0, at);
            for (const auto& use : record.uses)
                invalid(SsaIssue::ZeroValue, 0, use.at, {}, use.ordinal);
            continue;
        }
        if (record.definitions.empty()) {
            for (const auto& use : record.uses)
                invalid(SsaIssue::MissingDefinition, record.value, use.at, {}, use.ordinal);
            continue;
        }
        if (record.definitions.size() != 1) {
            // Keep every conflicting definition, including the first; there
            // is no privileged winner for checking this value's uses.
            for (const auto at : record.definitions)
                invalid(SsaIssue::MultipleDefinitions, record.value, at);
            continue;
        }
        const auto definition = record.definitions.front();
        for (const auto& use : record.uses) {
            if (definition.block == use.at.block) {
                if (definition.index >= use.at.index)
                    invalid(SsaIssue::DefinitionNotEarlier, record.value, use.at, definition, use.ordinal);
            } else if (!cfg.reachable[use.at.block]) {
                result.dominance_complete = false;
                result.unverified_dominance.push_back({record.value, use});
            } else if (!dom.dominates(definition.block, use.at.block)) {
                invalid(SsaIssue::DefinitionDoesNotDominate, record.value, use.at, definition, use.ordinal);
            }
        }
    }
    return result;
}
} // namespace quidra::ir
