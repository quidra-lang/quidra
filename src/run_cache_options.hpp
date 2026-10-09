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
// The artifact is keyed only when it is not an executable: a run always
// builds one, so the keys of runs stay as they were.

#include "quidra/compiler.hpp"
#include "toolchain/native_link_recipe.hpp"
#include "toolchain/run_cache_key.hpp"

#include <string>
#include <vector>

namespace quidra::run_cache {

inline std::vector<toolchain::RunCacheOption> cache_key_options(
    const CompileOptions& compile, const toolchain::LinkOptions& link) {
    const auto flag = [](bool value) { return std::string(value ? "true" : "false"); };
    const auto& [max_errors, debug_info, source_display_path, lowering, artifact] = compile;
    (void)max_errors;
    const auto& [reresolve_every_compound_store, copy_every_value_loop_at_entry,
                 check_every_reference_loop_statement, copy_every_borrowed_argument,
                 reorder_every_assignment] = lowering;
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
        {"link.debug", flag(debug)},
        {"link.optimize", flag(optimize)},
    };
    if (artifact != CompileArtifact::Executable) {
        options.push_back({"compile.artifact",
                           artifact == CompileArtifact::Library ? "library" : "interactive"});
    }
    return options;
}

} // namespace quidra::run_cache
