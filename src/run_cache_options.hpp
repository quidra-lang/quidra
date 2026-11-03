#pragma once

// The compile and link options of a direct run as run cache key material
// (toolchain/run_cache_key.hpp): every option that can change what a
// successful build produces, by name and value.
//
// Both option structs are read through structured bindings, so a new field
// of CompileOptions or LinkOptions stops the build here until the key says
// what it does with it. Left out, with the reason:
//   max_errors          only limits the diagnostics of a failed compile,
//                       which is never cached
//   inputs              each package native input and --link input is a
//                       dependency record with its content
//   pkg_config_modules  follow from the package manifests, which are
//                       recorded by content, and are part of the toolchain
//                       snapshot with pkg-config's output
//   optimizer.diagnostics receives analysis output and cannot change the
//                       produced executable; its address is never key material
//   optimizer.inspection receives observational reports; its address and
//                       capture flags are never executable key material
// The artifact is keyed only when it is not an executable: a run always
// builds one, so the keys of runs stay as they were. Unspecified optimizer
// modes keep the historical key fields; explicit optional modes, including
// Off, remain distinct because their defaults depend on other switches.

#include "quidra/compiler.hpp"
#include "toolchain/native_link_recipe.hpp"
#include "toolchain/run_cache_key.hpp"

#include <string>
#include <stdexcept>
#include <vector>

namespace quidra::run_cache {

inline std::vector<toolchain::RunCacheOption> cache_key_options(
    const CompileOptions& compile, const toolchain::LinkOptions& link) {
    const auto flag = [](bool value) { return std::string(value ? "true" : "false"); };
    const auto& [max_errors, debug_info, source_display_path, lowering, optimizer, artifact] =
        compile;
    (void)max_errors;
    const auto& [reresolve_every_compound_store, copy_every_value_loop_at_entry,
                 check_every_reference_loop_statement, copy_every_borrowed_argument,
                 reorder_every_assignment, check_every_shape_constraint,
                 exact_real_loop_values, box_every_address_taken_local,
                 check_every_reference_access] = lowering;
    const auto& [keep_every_finite_check, exact_binding, diagnostics, cfg_facts,
                 interproc, inline_functions, check_elimination, step_regions,
                 inspection] = optimizer;
    (void)diagnostics;
    (void)inspection;
    const auto& [debug, optimize, inputs, pkg_config_modules] = link;
    (void)inputs;
    (void)pkg_config_modules;
    std::vector<toolchain::RunCacheOption> options{
        {"compile.debug_info", flag(debug_info)},
        // Runtime failures name the root file as it was spelled, so
        // `run prog.qui` and `run ./prog.qui` never share an executable.
        {"compile.source_display_path", source_display_path},
        {"compile.lowering.reresolve_every_compound_store", flag(reresolve_every_compound_store)},
        {"compile.lowering.copy_every_value_loop_at_entry", flag(copy_every_value_loop_at_entry)},
        {"compile.lowering.check_every_reference_loop_statement",
         flag(check_every_reference_loop_statement)},
        {"compile.lowering.copy_every_borrowed_argument", flag(copy_every_borrowed_argument)},
        {"compile.lowering.reorder_every_assignment", flag(reorder_every_assignment)},
        {"compile.lowering.check_every_shape_constraint", flag(check_every_shape_constraint)},
        {"compile.lowering.exact_real_loop_values", flag(exact_real_loop_values)},
        {"compile.lowering.box_every_address_taken_local", flag(box_every_address_taken_local)},
        {"compile.lowering.check_every_reference_access", flag(check_every_reference_access)},
        {"link.debug", flag(debug)},
        {"link.optimize", flag(optimize)},
    };
    if (keep_every_finite_check)
        options.push_back({"compile.optimizer.keep_every_finite_check", "true"});
    if (exact_binding != DeclarationBindingMode::Off)
        options.push_back({"compile.optimizer.exact_binding",
            exact_binding == DeclarationBindingMode::Verify ? "verify" : "on"});
    const auto mode = [](ir::OptimizerMode value) -> std::string {
        switch (value) {
        case ir::OptimizerMode::Off: return "off";
        case ir::OptimizerMode::Verify: return "verify";
        case ir::OptimizerMode::On: return "on";
        }
        throw std::invalid_argument("unknown optimizer cache mode");
    };
    if (cfg_facts)
        options.push_back({"compile.optimizer.cfg_facts", mode(*cfg_facts)});
    if (interproc)
        options.push_back({"compile.optimizer.interproc", mode(*interproc)});
    if (inline_functions)
        options.push_back({"compile.optimizer.inline_functions", mode(*inline_functions)});
    if (check_elimination != ir::OptimizerMode::Off)
        options.push_back({"compile.optimizer.check_elimination", mode(check_elimination)});
    if (step_regions != ir::OptimizerMode::Off)
        options.push_back({"compile.optimizer.step_regions", mode(step_regions)});
    if (artifact != CompileArtifact::Executable) {
        options.push_back({"compile.artifact",
                           artifact == CompileArtifact::Library ? "library" : "interactive"});
    }
    return options;
}

} // namespace quidra::run_cache
