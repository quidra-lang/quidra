#pragma once

// ir::optimize: the optimizer's pass pipeline over a module (src/optimizer).

#include "quidra/ir/module.hpp"
#include "quidra/declaration_identity.hpp"

#include <string>
#include <cstdint>
#include <optional>
#include <vector>

namespace quidra::optimizer { struct OptimizerInspection; }

namespace quidra::ir {

enum class OptimizerMode : std::uint8_t { Off, Verify, On };

struct OptimizerDiagnostic {
    enum class Kind { BindingDifference, BindingIssue, UnavailableRule, UnavailableAnalysis } kind;
    std::string reference, reason;
    std::vector<std::string> legacy, exact;
};

// Internal optimizer switches for tests (tests/lowering_switch_tests.sh).
struct OptimizerOptions {
    // Every finite check of real arithmetic stays where the lowering wrote
    // it: none is merged with another or sunk out of a loop.
    bool keep_every_finite_check{};
    DeclarationBindingMode exact_binding{DeclarationBindingMode::Off};
    // Optional owned output for verification differences and declined rules.
    std::vector<OptimizerDiagnostic>* diagnostics{};
    // Unspecified interprocedural switches retain the original exact-binding
    // API behavior. Explicit modes provide independent compile-time controls.
    std::optional<OptimizerMode> cfg_facts;
    std::optional<OptimizerMode> interproc;
    std::optional<OptimizerMode> inline_functions;
    OptimizerMode check_elimination{OptimizerMode::Off};
    OptimizerMode step_regions{OptimizerMode::Off};
    // Observational output only; never an externally supplied proof source.
    optimizer::OptimizerInspection* inspection{};
};

Module optimize(Module module, OptimizerOptions options = {});

} // namespace quidra::ir
