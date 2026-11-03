#include "ir_cli.hpp"
#include "ir_report.hpp"
#include "quidra/ir.hpp"

#include <ostream>
#include <stdexcept>
#include <utility>

namespace quidra::cli {
IrCommandOptions parse_ir_options(const std::vector<std::string>& arguments) {
    IrCommandOptions result;
    bool options = true;
    for (const auto& argument : arguments) {
        if (options && argument == "--") { options = false; continue; }
        if (options && parse_optimization_option(argument, result.optimization)) continue;
        if (options && argument == "--proofs") result.proofs = true;
        else if (options && argument == "--step-regions") result.step_regions = true;
        else if (options && argument.starts_with("--"))
            throw std::runtime_error("unknown ir option: " + argument);
        else {
            if (!result.source.empty()) throw std::runtime_error("quidra ir requires exactly one source file");
            result.source = argument;
        }
    }
    if (result.source.empty()) throw std::runtime_error("quidra ir requires a source file");
    return result;
}
void collect_unobserved_ir_reports(const ir::Module& module,
    optimizer::OptimizerInspection& inspection) {
    if (inspection.capture_proofs && !inspection.proofs_observed) {
        for (std::size_t index = 0; index < module.functions.size(); ++index) {
            const auto& function = module.functions[index];
            const auto plan = optimizer::CheckElimination::analyze(module, index);
            optimizer::ProofInspection row;
            row.function = index; row.lookup = function.name; row.declaration = function.declaration;
            row.site_context = function; row.origin = optimizer::InspectionOrigin::Collector;
            row.ready = plan.ready(); row.report = plan.report();
            inspection.proofs.push_back(std::move(row));
        }
    }
    if (inspection.capture_step_regions && !inspection.step_regions_observed) {
        for (std::size_t index = 0; index < module.functions.size(); ++index) {
            const auto& function = module.functions[index];
            optimizer::StepRegionInspection row;
            row.function = index; row.lookup = function.name; row.declaration = function.declaration;
            row.site_context = function; row.origin = optimizer::InspectionOrigin::Collector;
            row.report = ir::analysis::StepRegionFormation::analyze(module, index);
            inspection.step_regions.push_back(std::move(row));
        }
    }
}
int run_ir_cli(const IrCommandOptions& options, const IrCompiler& compiler,
    std::ostream& output) {
    optimizer::OptimizerInspection inspection;
    inspection.capture_proofs = options.proofs;
    inspection.capture_step_regions = options.step_regions;
    ir::OptimizerOptions optimizer_options;
    apply_optimization_settings(options.optimization, optimizer_options);
    if (options.proofs || options.step_regions) optimizer_options.inspection = &inspection;
    auto module = compiler(options.source, optimizer_options);
    collect_unobserved_ir_reports(module, inspection);
    // Prepare whole documents before writing any diagnostic line.
    const auto proofs = options.proofs ? ir_proofs_report(inspection) : std::string{};
    const auto regions = options.step_regions ? ir_step_regions_report(inspection) : std::string{};
    output << ir::dump(module);
    if (options.proofs) output << "proofs " << proofs << '\n';
    if (options.step_regions) output << "step_regions " << regions << '\n';
    return 0;
}
} // namespace quidra::cli
