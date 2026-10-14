#pragma once

#include "optimizer/inspection.hpp"
#include "optimization_cli.hpp"
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace quidra::cli {
struct IrCommandOptions {
    std::filesystem::path source;
    bool proofs{}, step_regions{};
    OptimizationSettings optimization;
};
IrCommandOptions parse_ir_options(const std::vector<std::string>&);
using IrCompiler = std::function<ir::Module(const std::filesystem::path&,
    const ir::OptimizerOptions&)>;
// The runner preserves the compiler's options/defaults. A null sink is used
// when no report was requested. Root-owned pass capture fills observed rows.
int run_ir_cli(const IrCommandOptions&, const IrCompiler&, std::ostream&);
// Only channels not observed by the optimizer use current-module analysis.
// These rows remain collector-origin and do not apply proposed changes.
void collect_unobserved_ir_reports(const ir::Module&,
    optimizer::OptimizerInspection&);
} // namespace quidra::cli
