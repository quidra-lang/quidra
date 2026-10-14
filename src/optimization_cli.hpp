#pragma once

#include "quidra/optimizer.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::cli {
enum class OptimizationKey : std::uint8_t {
    CfgFacts, ExactBinding, Interproc, Inline, CheckElimination, StepRegions
};
using OptimizationSettings = std::map<OptimizationKey, ir::OptimizerMode>;
std::string_view optimization_key_name(OptimizationKey);
std::string_view optimization_mode_name(ir::OptimizerMode);
// Returns false for other options. Malformed/unknown --opt settings throw.
bool parse_optimization_option(std::string_view, OptimizationSettings&);
// Unspecified settings retain the supplied options and their API coupling.
void apply_optimization_settings(const OptimizationSettings&, ir::OptimizerOptions&);
// Stable key order; original argument order/spelling is never a mode identity.
std::vector<std::string> normalized_optimization_options(const OptimizationSettings&);
} // namespace quidra::cli
