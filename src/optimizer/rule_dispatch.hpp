#pragma once
#include "optimizer/rule_registry.hpp"
#include <cstddef>
#include <string>
#include <vector>
namespace quidra::ir::analysis { class AnalysisCache; }
namespace quidra::optimizer {
struct UnavailableRule { std::string reference, reason; };
struct RuleDispatchReport {
    std::size_t rewritten_calls{}, generated_stubs{};
    std::vector<UnavailableRule> unavailable;
    bool changed() const noexcept { return rewritten_calls != 0; }
};
class RuleDispatchPass {
public:
    // Run after chain/legacy rewrites and before finite/region passes. All
    // rewrites are collected before append; the caller must refresh indexes,
    // summaries and analyses after a changed report.
    static RuleDispatchReport run(ir::Module&, const RuleRegistry&,
                                  ir::analysis::AnalysisCache* = nullptr);
};
} // namespace quidra::optimizer
