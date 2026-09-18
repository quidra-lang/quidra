#include "repl_cli.hpp"

#include "quidra/compiler.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"
#include "quidra/types.hpp"
#include "quidra/version.hpp"
#include "native_build.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::cli {
namespace {

constexpr std::string_view repl_begin = "__QUIDRA_REPL_RESULT_BEGIN_6D8F2C__";
constexpr std::string_view repl_end = "__QUIDRA_REPL_RESULT_END_6D8F2C__";
volatile std::sig_atomic_t repl_interrupted = 0;

void handle_sigint(int) {
    repl_interrupted = 1;
#ifndef _WIN32
    const char newline = '\n';
    const auto written = ::write(STDOUT_FILENO, &newline, 1);
    (void)written;
#endif
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write file: " + path.string());
    out << text;
}

class ReplFiles {
public:
    ReplFiles() {
        std::random_device rd;
        const auto now = static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto nonce =
                (static_cast<unsigned long long>(rd()) << 32U) ^ rd() ^ now ^ attempt;
            source_ = fs::current_path() /
                (".quidra-repl-" + std::to_string(nonce) + ".qui");
            std::error_code ec;
            if (!fs::exists(source_, ec)) break;
            source_.clear();
        }
        if (source_.empty()) throw std::runtime_error("cannot create REPL source path");

        temp_ = fs::temp_directory_path() /
            (".quidra-repl-build-" + std::to_string(now) + "-" + std::to_string(rd()));
        if (!fs::create_directory(temp_)) {
            throw std::runtime_error("cannot create REPL build directory");
        }
    }

    ~ReplFiles() {
        std::error_code ec;
        fs::remove(source_, ec);
        fs::remove_all(temp_, ec);
    }

    const fs::path& source() const { return source_; }
    fs::path llvm() const { return temp_ / "submission.ll"; }
    fs::path executable() const {
#ifdef _WIN32
        return temp_ / "submission.exe";
#else
        return temp_ / "submission";
#endif
    }
    fs::path stdout_file() const { return temp_ / "stdout.txt"; }
    fs::path stderr_file() const { return temp_ / "stderr.txt"; }

private:
    fs::path source_;
    fs::path temp_;
};

struct NativeResult {
    int status{};
    std::string stdout_text;
    std::string stderr_text;
};

int compile_native(const Compilation& compilation, const ReplFiles& files) {
    write_file(files.llvm(), compilation.llvm);
    return native::link_llvm(files.llvm(), files.executable());
}

NativeResult run_native(const ReplFiles& files) {
    const auto rc = native::run_program(
        files.executable(), {}, files.stdout_file(), files.stderr_file());

    NativeResult result;
    result.status = rc;
    result.stdout_text = read_file(files.stdout_file());
    result.stderr_text = read_file(files.stderr_file());
    return result;
}

void print_diagnostics(const CompileErrors& errors) {
    for (const auto& diagnostic : errors.diagnostics()) {
        std::cerr << "<repl>:" << diagnostic.span.start.line << ":"
                  << diagnostic.span.start.column << ": error[" << diagnostic.code
                  << "] " << diagnostic.message << "\n";
    }
    std::cerr << errors.diagnostics().size() << " error(s) generated.";
    if (errors.truncated()) std::cerr << " Further diagnostics suppressed.";
    std::cerr << "\n";
}

void print_diagnostic(const CompileError& error) {
    const auto& diagnostic = error.diagnostic();
    std::cerr << "<repl>:" << diagnostic.span.start.line << ":"
              << diagnostic.span.start.column << ": error[" << diagnostic.code
              << "] " << diagnostic.message << "\n";
}

std::string trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) ++first;
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
    return std::string(text.substr(first, last - first));
}

bool starts_with_word(const std::string& text, std::string_view word) {
    return text == word ||
           (text.size() > word.size() && text.compare(0, word.size(), word) == 0 &&
            text[word.size()] == ' ');
}

bool block_header(std::string_view line) {
    const auto text = trim(line);
    if (starts_with_word(text, "class") || starts_with_word(text, "if") ||
        starts_with_word(text, "while") || starts_with_word(text, "for") ||
        starts_with_word(text, "match")) {
        return true;
    }

    const auto open = text.find('(');
    if (open == std::string::npos || text.find('=', 0) < open) return false;
    const auto close = text.rfind(')');
    if (close == std::string::npos || close < open) return false;
    const auto prefix = trim(std::string_view(text).substr(0, open));
    return prefix.find(' ') != std::string::npos;
}

bool lexically_incomplete(std::string_view text) {
    int parens = 0;
    int brackets = 0;
    bool in_string = false;
    for (char c : text) {
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (c == '(') ++parens;
        else if (c == ')') --parens;
        else if (c == '[') ++brackets;
        else if (c == ']') --brackets;
    }
    return in_string || parens > 0 || brackets > 0;
}

struct ReplOutput {
    std::string normal;
    std::string display;
};

ReplOutput split_repl_output(const std::string& output) {
    const std::string begin = std::string(repl_begin) + "\n";
    const std::string end = std::string(repl_end) + "\n";
    const auto start = output.rfind(begin);
    if (start == std::string::npos) return {output, {}};
    const auto finish = output.find(end, start + begin.size());
    if (finish == std::string::npos) return {output, {}};

    ReplOutput result;
    result.normal = output.substr(0, start);
    result.normal += output.substr(finish + end.size());
    result.display = output.substr(start + begin.size(), finish - (start + begin.size()));
    return result;
}

bool replay_barrier_instruction(const ir::Instruction& instruction) {
    return std::visit([](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        return
            std::is_same_v<T, ir::Input> ||
            std::is_same_v<T, ir::Exit> ||
            std::is_same_v<T, ir::CliArgument> ||
            std::is_same_v<T, ir::CliOption> ||
            std::is_same_v<T, ir::CliFlag> ||
            std::is_same_v<T, ir::CliFinish> ||
            std::is_same_v<T, ir::FileRead> ||
            std::is_same_v<T, ir::FileReadBin> ||
            std::is_same_v<T, ir::FileWrite> ||
            std::is_same_v<T, ir::FileWriteBin> ||
            std::is_same_v<T, ir::FileExists> ||
            std::is_same_v<T, ir::FileIsDirectory> ||
            std::is_same_v<T, ir::FileRemove> ||
            std::is_same_v<T, ir::FileCopy> ||
            std::is_same_v<T, ir::FileMove> ||
            std::is_same_v<T, ir::FileMkdir> ||
            std::is_same_v<T, ir::FileList> ||
            std::is_same_v<T, ir::EnvironmentGet> ||
            std::is_same_v<T, ir::EnvironmentHas> ||
            std::is_same_v<T, ir::TimeNow> ||
            std::is_same_v<T, ir::TimeSince> ||
            std::is_same_v<T, ir::TimeSleep> ||
            std::is_same_v<T, ir::RandomGenerator> ||
            std::is_same_v<T, ir::RandomInt> ||
            std::is_same_v<T, ir::RandomFloat> ||
            std::is_same_v<T, ir::RandomBool> ||
            std::is_same_v<T, ir::ProcessRun> ||
            std::is_same_v<T, ir::HttpGet> ||
            std::is_same_v<T, ir::ImageRead> ||
            std::is_same_v<T, ir::ImageWrite> ||
            std::is_same_v<T, ir::NeuralSave> ||
            std::is_same_v<T, ir::NeuralLoad>;
    }, instruction);
}

bool module_requires_replay_barrier(const ir::Module& module) {
    std::unordered_map<std::string, const ir::Function*> functions;
    const ir::Function* entrypoint = nullptr;
    for (const auto& function : module.functions) {
        functions.emplace(function.name, &function);
        if (function.entrypoint) entrypoint = &function;
    }
    if (!entrypoint) return true;

    std::vector<const ir::Function*> pending{entrypoint};
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        const auto* function = pending.back();
        pending.pop_back();
        if (!visited.insert(function->name).second) continue;
        if (function->external_symbol) return true;
        for (const auto& block : function->blocks) {
            for (const auto& instruction : block.instructions) {
                if (replay_barrier_instruction(instruction)) return true;
                if (const auto* call = std::get_if<ir::Call>(&instruction)) {
                    const auto target = functions.find(call->callee);
                    if (target == functions.end()) return true;
                    pending.push_back(target->second);
                }
            }
        }
    }
    return false;
}

bool declaration_only_submission(std::string_view source) {
    try {
        auto parsed=Parser(Lexer(source).scan()).parse();
        return parsed.statements.empty();
    } catch (const CompileError&) {
        return false;
    } catch (const CompileErrors&) {
        return false;
    }
}

void repl_help() {
    std::cout
        << "Quidra REPL commands:\n"
        << "  :help             show this help\n"
        << "  :type EXPR        type-check EXPR and print its type\n"
        << "  :reset            clear accepted session state\n"
        << "  :quit, :exit      leave the REPL\n"
        << "Ctrl-D exits. Ctrl-C cancels the current input.\n";
}

class ReplSession {
public:
    ReplSession() : working_directory_(fs::current_path()) {}

    bool submit(const std::string& submission) {
        const bool declaration_only=declaration_only_submission(submission);
        if (replay_barrier_ && !declaration_only) {
            std::cerr
                << "quidra: error[REPL_REPLAY_UNSAFE] the current accumulated-source "
                   "session may have executed external or nondeterministic effects; "
                   "use :reset before executing another executable submission\n";
            return false;
        }
        const auto replay_prefix_bytes = accepted_source_.size();
        std::string candidate = accepted_source_;
        candidate += submission;
        if (candidate.empty() || candidate.back() != '\n') candidate += '\n';
        write_file(files_.source(), candidate);

        try {
            if (declaration_only) {
                // Declarations/imports change compile-time state only. Validate the complete
                // candidate through the ordinary file-aware checker, then retain the source
                // without LLVM generation, native linking, or process execution.
                (void)check_file_source(files_.source(), candidate, {}, working_directory_);
                accepted_source_ = std::move(candidate);
                return true;
            }

            auto compiled = compile_repl_file(
                files_.source(), {}, working_directory_, replay_prefix_bytes);
            const bool candidate_replay_barrier =
                module_requires_replay_barrier(compiled.compilation.ir);
            if (compile_native(compiled.compilation, files_) != 0) return false;

            // Once native execution can reach an external or nondeterministic
            // effect, a failing submission is no longer safely retryable: the
            // effect may have happened before the runtime error. Arm the barrier
            // before launch and keep it even when the candidate is not accepted.
            if (candidate_replay_barrier) replay_barrier_ = true;

            auto native = run_native(files_);
            auto output = split_repl_output(native.stdout_text);

            std::cout << output.normal;
            if (!output.display.empty()) std::cout << output.display;
            if (!native.stderr_text.empty()) std::cerr << native.stderr_text;

            if (native.status != 0) return false;
            accepted_source_ = std::move(candidate);
            replay_barrier_ = candidate_replay_barrier;
            return true;
        } catch (const CompileErrors& error) {
            print_diagnostics(error);
        } catch (const CompileError& error) {
            print_diagnostic(error);
        } catch (const std::exception& error) {
            std::cerr << "quidra: " << error.what() << "\n";
        }
        return false;
    }

    void reset() {
        accepted_source_.clear();
        replay_barrier_ = false;
        std::cout << "REPL state reset.\n";
    }

    void print_type(std::string_view expression) {
        std::string candidate = accepted_source_;
        candidate += expression;
        if (candidate.empty() || candidate.back() != '\n') candidate += '\n';
        write_file(files_.source(), candidate);

        try {
            auto compiled = compile_repl_file(files_.source(), {}, working_directory_);
            if (!compiled.expression_type) {
                std::cerr << "quidra: :type requires an expression\n";
                return;
            }
            std::cout << type_name(*compiled.expression_type) << "\n";
        } catch (const CompileErrors& error) {
            print_diagnostics(error);
        } catch (const CompileError& error) {
            print_diagnostic(error);
        } catch (const std::exception& error) {
            std::cerr << "quidra: " << error.what() << "\n";
        }
    }

private:
    fs::path working_directory_;
    ReplFiles files_;
    std::string accepted_source_;
    bool replay_barrier_{};
};

} // namespace

int run_repl() {
#ifdef _WIN32
    const auto previous = std::signal(SIGINT, handle_sigint);
#else
    struct sigaction action {};
    struct sigaction previous {};
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, &previous);
#endif

    const auto restore_signal = [&]() {
#ifdef _WIN32
        std::signal(SIGINT, previous);
#else
        sigaction(SIGINT, &previous, nullptr);
#endif
    };

    std::cout << "Quidra " << compiler_version << "\n";
    ReplSession session;
    std::string submission;
    bool block = false;

    while (true) {
        repl_interrupted = 0;
        std::cout << (submission.empty() ? ">>> " : "... ") << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            if (repl_interrupted) {
                std::cin.clear();
                submission.clear();
                block = false;
                continue;
            }
            std::cout << "\n";
            restore_signal();
            return 0;
        }

        if (repl_interrupted) {
            submission.clear();
            block = false;
            continue;
        }

        if (submission.empty() && !line.empty() && line.front() == ':') {
            if (line == ":quit" || line == ":exit") {
                restore_signal();
                return 0;
            }
            if (line == ":help") {
                repl_help();
                continue;
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
            std::cerr << "quidra: unknown REPL command; use :help\n";
            continue;
        }

        // A blank line with nothing pending carries no code. Submitting it would
        // append an empty line to the accepted source and re-run the whole
        // accumulated session, repeating every side effect already produced.
        if (submission.empty() && trim(line).empty()) continue;

        if (submission.empty()) block = block_header(line);

        if (block && line.empty()) {
            if (!lexically_incomplete(submission) && !submission.empty()) {
                session.submit(submission);
                submission.clear();
                block = false;
            }
            continue;
        }

        submission += line;
        submission += '\n';

        if (!block && !lexically_incomplete(submission)) {
            session.submit(submission);
            submission.clear();
        }
    }
}

} // namespace quidra::cli
