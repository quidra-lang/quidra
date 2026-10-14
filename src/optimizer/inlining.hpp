#pragma once
#include "quidra/ir/module.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace quidra::optimizer {
class RuleRegistry;
struct InlineHotSite { std::string caller; std::size_t block{}, instruction{}; };
struct InliningOptions {
    std::size_t ordinary_instruction_limit{80}, hot_instruction_limit{400};
    std::uint64_t maximum_operations{2000000};
    std::vector<InlineHotSite> hot_sites;
    // Exact original backend cleanup modes; no generated-name inference.
    std::optional<std::map<std::string,bool,std::less<>>> node_cleanup_modes;
};
struct InlineUnavailable { std::string caller,callee,reason; };
struct InliningReport {
    std::size_t inlined_calls{},instruction_growth{};
    std::uint64_t operations{},budget_fallbacks{};
    std::vector<InlineUnavailable> unavailable;
    bool changed() const noexcept {return inlined_calls!=0;}
};
class BudgetedInliner {
public:
    // The caller refreshes indices/facts/entry meets after changes. Backend
    // scope support must be registered before this otherwise-unused pass runs.
    static InliningReport run(ir::Module&,const RuleRegistry&,InliningOptions={});
};
} // namespace quidra::optimizer
