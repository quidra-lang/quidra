#pragma once

#include "ir/facts/tensor.hpp"
#include "ir/summaries.hpp"

namespace quidra::ir::facts {
struct FunctionFactSource {
    // Exact S1/L17 public semantic coordinates, copied during lowering.
    std::vector<ExtentOrigin> result_extents;
    std::vector<std::optional<std::size_t>> semantic_parameter_for_ir_parameter;
};
struct PolicyAssignment {
    std::string identity, value;
    bool operator==(const PolicyAssignment&) const = default;
};
struct PolicyContracts {
    // Exact bound declarations supplied by the registry, never name guesses.
    std::map<std::string, std::vector<PolicyAssignment>, std::less<>> setters;
    std::map<std::string, bool, std::less<>> writes_policy;
};
struct FactSources {
    std::map<std::string, FunctionFactSource, std::less<>> functions;
    PolicyContracts policies;
    const FunctionFactSource* find(std::string_view name) const noexcept;
};
using ResultExtentSources = std::map<std::string, std::vector<ExtentOrigin>, std::less<>>;
FactSources adapt_fact_sources(const Module&, const analysis::SummaryBindings&, const ResultExtentSources&);
} // namespace quidra::ir::facts
