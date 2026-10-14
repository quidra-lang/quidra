#pragma once

// ir::optimize: the optimizer's pass pipeline over a module (src/optimizer).

#include "quidra/ir/module.hpp"
#include "quidra/declaration_identity.hpp"

#include <string>
#include <vector>

namespace quidra::ir {

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
};

Module optimize(Module module, OptimizerOptions options = {});

} // namespace quidra::ir
