// The golden view `repl.session`: the compile path of `quidra repl < FILE`
// (README.md, Inputs).
//
// The simulator reads its input the way run_repl() reads redirected stdin and
// replays ReplSession::submit, :reset and :type with the REPL's own helpers
// (src/repl_submission.hpp): has_repl_command decides between one submission
// and the line loop, the REPL's SubmissionAssembler cuts the lines into
// submissions, declaration_only_submission classifies them, they compile with
// accepted source + submission and replay_prefix_bytes, and the replay
// barrier comes from module_requires_replay_barrier. It does not run the JIT:
// every submission that compiles is taken as accepted.
#pragma once

#include "quidra/checker.hpp"
#include "quidra/ir.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::golden {

// The modules of the compiled submissions, for the census.
struct SessionModules {
    std::vector<ir::Module> lowered;
    std::vector<ir::Module> optimized;
};

// The expression the REPL displays: the last statement of the program when it
// is an expression statement, as finish_repl_compile (src/compiler.cpp)
// chooses it; null otherwise.
const Expr* displayed_expression(const CheckedProgram& checked);

// One record per submission or meta-command: its kind, status, and for an
// executable submission the expression type, ir.full and LLVM text. With
// `modules`, also keeps every compiled submission's lowered (ir::lower with
// the REPL expression and replay prefix) and optimized module.
std::string simulate_repl_session(std::string_view input, const std::filesystem::path& cwd,
                                  SessionModules* modules = nullptr);

} // namespace quidra::golden
