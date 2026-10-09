// REPL session simulator; see repl_session.hpp.
#include "repl_session.hpp"

#include "failure.hpp"
#include "ir_full.hpp"
#include "repl_submission.hpp"

#include "quidra/compiler.hpp"
#include "quidra/project.hpp"

#include <sstream>
#include <string>
#include <variant>

namespace quidra::golden {
namespace {

namespace fs = std::filesystem;

class SessionSimulator {
public:
    SessionSimulator(fs::path cwd, SessionModules* modules)
        : cwd_(std::move(cwd)),
          source_(cwd_ / source_filename(".quidra-repl-golden")),
          modules_(modules) {}

    // ReplSession::submit, without the JIT run.
    void submit(const std::string& submission) {
        const bool declaration_only = cli::declaration_only_submission(submission);
        record("submit", declaration_only ? "declaration" : "executable");
        out_ += "source " + quote(submission) + "\n";
        if (replay_barrier_ && !declaration_only) {
            out_ += "status rejected REPL_REPLAY_UNSAFE\n";
            return;
        }
        const auto replay_prefix_bytes = accepted_source_.size();
        std::string candidate = accepted_source_;
        candidate += submission;
        if (candidate.empty() || candidate.back() != '\n') candidate += '\n';
        out_ += "replay_prefix_bytes " + std::to_string(replay_prefix_bytes) + "\n";
        try {
            if (declaration_only) {
                (void)check_file_source(source_, candidate, {}, cwd_);
                accepted_source_ = std::move(candidate);
                out_ += "status accepted\n";
                return;
            }
            auto compiled =
                compile_repl_file_source(source_, candidate, {}, cwd_, replay_prefix_bytes);
            const bool barrier = cli::module_requires_replay_barrier(compiled.compilation.ir);
            out_ += "status compiled barrier=" + std::string(barrier ? "1" : "0") + "\n";
            out_ += "expression_type ";
            out_ += compiled.expression_type ? serialize_type(*compiled.expression_type) : "none";
            out_ += "\n--- ir.full\n" + serialize_module(compiled.compilation.ir);
            out_ += "--- llvm\n" + compiled.compilation.llvm;
            out_ += "--- end\n";
            if (modules_) record_modules(replay_prefix_bytes, compiled);
            // The REPL runs the module here and accepts the candidate when the
            // run succeeds; the simulator assumes it does.
            accepted_source_ = std::move(candidate);
            replay_barrier_ = barrier;
        } catch (...) {
            const auto failure = current_failure();
            out_ += "status " + failure.status + "\n" + failure.text;
        }
    }

    void reset() {
        record("reset", "");
        accepted_source_.clear();
        replay_barrier_ = false;
    }

    // ReplSession::print_type.
    void print_type(std::string_view expression) {
        record("type", "");
        out_ += "source " + quote(expression) + "\n";
        std::string candidate = accepted_source_;
        candidate += expression;
        if (candidate.empty() || candidate.back() != '\n') candidate += '\n';
        try {
            const auto checked = check_file_source(source_, candidate, {}, cwd_);
            const Expr* expression_node = displayed_expression(checked);
            if (!expression_node) {
                out_ += "status no-expression\n";
                return;
            }
            const auto found = checked.expr_types.find(expression_node);
            out_ += "status ok\ntype " +
                    (found == checked.expr_types.end() ? std::string("missing")
                                                       : serialize_type(found->second)) +
                    "\n";
        } catch (...) {
            const auto failure = current_failure();
            out_ += "status " + failure.status + "\n" + failure.text;
        }
    }

    void command(std::string_view line) {
        record("command", "");
        out_ += "source " + quote(line) + "\n";
    }

    std::string take() { return std::move(out_); }

private:
    // The lowered module of a compiled submission: ir::lower over the checked
    // program the compilation kept (read by const&, never copied), with the
    // displayed expression and replay prefix finish_repl_compile used.
    void record_modules(std::size_t replay_prefix_bytes, ReplCompilation& compiled) {
        const CheckedProgram& checked = compiled.compilation.checked;
        modules_->lowered.push_back(
            ir::lower(checked, displayed_expression(checked), replay_prefix_bytes));
        modules_->optimized.push_back(std::move(compiled.compilation.ir));
    }

    void record(std::string_view kind, std::string_view detail) {
        out_ += "#" + std::to_string(count_++) + " " + std::string(kind);
        if (!detail.empty()) out_ += " " + std::string(detail);
        out_ += "\n";
    }

    fs::path cwd_;
    fs::path source_;
    std::string accepted_source_;
    bool replay_barrier_{};
    std::size_t count_{};
    std::string out_;
    SessionModules* modules_{};
};

} // namespace

const Expr* displayed_expression(const CheckedProgram& checked) {
    if (checked.program.statements.empty()) return nullptr;
    const auto& last = checked.program.statements.back();
    const auto* statement = std::get_if<ExprStmt>(&last->data);
    return statement ? statement->value.get() : nullptr;
}

// run_repl() for redirected input: a file without meta-commands is one
// submission; otherwise lines are read as at the interactive prompt.
std::string simulate_repl_session(std::string_view input, const fs::path& cwd,
                                  SessionModules* modules) {
    SessionSimulator session(cwd, modules);
    const std::string source(input);

    if (!cli::has_repl_command(source)) {
        if (source.find_first_not_of(" \t\r\n") != std::string::npos) session.submit(source);
        return session.take();
    }

    std::istringstream lines(source);
    cli::SubmissionAssembler assembler;
    std::string line;
    while (std::getline(lines, line)) {
        if (!assembler.pending() && !line.empty() && line.front() == ':') {
            if (line == ":quit" || line == ":exit") {
                session.command(line);
                break;
            }
            if (line == ":reset") {
                session.reset();
                continue;
            }
            constexpr std::string_view type_prefix = ":type ";
            if (line.rfind(type_prefix, 0) == 0) {
                session.print_type(std::string_view(line).substr(type_prefix.size()));
                continue;
            }
            session.command(line);
            continue;
        }
        if (auto submission = assembler.add_line(line)) session.submit(*submission);
    }
    return session.take();
}

} // namespace quidra::golden
