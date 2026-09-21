#include "repl_cli.hpp"

#include "quidra/compiler.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"
#include "quidra/types.hpp"
#include "quidra/version.hpp"
#include "native_build.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
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

NativeResult run_external_jit(
    const Compilation& compilation, const ReplFiles& files) {
    write_file(files.llvm(), compilation.llvm);
    const auto rc = native::run_llvm_jit(
        files.llvm(), {}, native::JitOptions{false, "<repl>"},
        files.stdout_file(), files.stderr_file());

    NativeResult result;
    result.status = rc;
    result.stdout_text = read_file(files.stdout_file());
    result.stderr_text = read_file(files.stderr_file());
    return result;
}

#ifndef _WIN32
class PersistentReplJit {
public:
    explicit PersistentReplJit(const ReplFiles& files) : files_(files) {}
    PersistentReplJit(const PersistentReplJit&) = delete;
    PersistentReplJit& operator=(const PersistentReplJit&) = delete;

    ~PersistentReplJit() { stop(); }

    NativeResult run(const Compilation& compilation) {
        if (disabled_)
            return external_fallback(compilation);

        if (!ensure_started()) {
            disabled_ = true;
            return external_fallback(compilation);
        }

        const std::string header =
            "RUN " + std::to_string(compilation.llvm.size()) + "\n";
        if (!write_all(input_fd_, header) ||
            !write_all(input_fd_, compilation.llvm)) {
            return server_terminated_result("persistent JIT request failed");
        }

        std::string response;
        if (!read_line(output_fd_, response))
            return server_terminated_result("persistent JIT terminated");

        if (response.rfind("OK ", 0) == 0) {
            NativeResult result;
            try {
                result.status = std::stoi(response.substr(3));
            } catch (...) {
                return protocol_failure("invalid persistent JIT status");
            }
            result.stdout_text = read_file(files_.stdout_file());
            result.stderr_text = read_file(files_.stderr_file());
            return result;
        }

        const bool safe_fallback = response.rfind("ERR ", 0) == 0;
        const bool post_execution = response.rfind("POSTERR ", 0) == 0;
        if (safe_fallback || post_execution) {
            const auto prefix = safe_fallback ? 4U : 8U;
            std::size_t message_size = 0;
            try {
                message_size =
                    static_cast<std::size_t>(std::stoull(response.substr(prefix)));
            } catch (...) {
                return protocol_failure("invalid persistent JIT error response");
            }
            std::string message(message_size, '\0');
            if (!read_exact(output_fd_, message.data(), message.size()))
                return protocol_failure("truncated persistent JIT error response");

            if (safe_fallback) {
                // ERR is emitted only before the generated entrypoint starts,
                // so replay through the established lli path cannot duplicate
                // user-visible side effects.
                fast_path_error_ = std::move(message);
                disabled_ = true;
                stop();
                return external_fallback(compilation);
            }

            NativeResult result;
            result.status = 1;
            result.stdout_text = read_file(files_.stdout_file());
            result.stderr_text = read_file(files_.stderr_file());
            if (!result.stderr_text.empty() &&
                result.stderr_text.back() != '\n')
                result.stderr_text.push_back('\n');
            result.stderr_text +=
                "quidra: JIT execution failed: " + message + "\n";
            return result;
        }

        return protocol_failure("invalid persistent JIT response");
    }

private:
    NativeResult external_fallback(const Compilation& compilation) {
        try {
            return run_external_jit(compilation, files_);
        } catch (const std::exception& fallback_error) {
            if (!fast_path_error_.empty()) {
                throw std::runtime_error(
                    "persistent JIT unavailable: " + fast_path_error_ +
                    "; lli fallback failed: " + fallback_error.what());
            }
            throw;
        }
    }

    static bool write_all(int fd, std::string_view data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written =
                ::write(fd, data.data() + offset, data.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (written == 0) return false;
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    static bool read_exact(int fd, char* data, std::size_t size) {
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = ::read(fd, data + offset, size - offset);
            if (count < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (count == 0) return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    static bool read_line(int fd, std::string& line) {
        line.clear();
        char c = 0;
        while (true) {
            const auto count = ::read(fd, &c, 1);
            if (count < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (count == 0) return false;
            if (c == '\n') return true;
            line.push_back(c);
            if (line.size() > 4096) return false;
        }
    }

    bool ensure_started() {
        if (pid_ > 0) return true;

        int request_pipe[2]{-1, -1};
        int response_pipe[2]{-1, -1};
        if (::pipe(request_pipe) != 0) return false;
        if (::pipe(response_pipe) != 0) {
            ::close(request_pipe[0]);
            ::close(request_pipe[1]);
            return false;
        }

        const auto executable = native::self_executable();
        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(request_pipe[0]);
            ::close(request_pipe[1]);
            ::close(response_pipe[0]);
            ::close(response_pipe[1]);
            pid_ = -1;
            return false;
        }

        if (pid_ == 0) {
            ::dup2(request_pipe[0], STDIN_FILENO);
            ::dup2(response_pipe[1], STDOUT_FILENO);
            ::close(request_pipe[0]);
            ::close(request_pipe[1]);
            ::close(response_pipe[0]);
            ::close(response_pipe[1]);

            const auto executable_text = executable.string();
            const auto stdout_text = files_.stdout_file().string();
            const auto stderr_text = files_.stderr_file().string();
            ::execl(
                executable_text.c_str(), executable_text.c_str(),
                "__jit-server",
                stdout_text.c_str(), stderr_text.c_str(),
                static_cast<char*>(nullptr));
            ::_exit(127);
        }

        ::close(request_pipe[0]);
        ::close(response_pipe[1]);
        input_fd_ = request_pipe[1];
        output_fd_ = response_pipe[0];

        std::string ready;
        if (!read_line(output_fd_, ready)) {
            fast_path_error_ = "persistent JIT worker exited before READY";
            stop();
            return false;
        }
        if (ready == "READY") return true;

        if (ready.rfind("STARTERR ", 0) == 0) {
            std::size_t message_size = 0;
            try {
                message_size =
                    static_cast<std::size_t>(std::stoull(ready.substr(9)));
            } catch (...) {
                fast_path_error_ = "invalid persistent JIT startup response";
                stop();
                return false;
            }
            std::string message(message_size, '\0');
            if (!read_exact(output_fd_, message.data(), message.size())) {
                fast_path_error_ = "truncated persistent JIT startup response";
                stop();
                return false;
            }
            fast_path_error_ = std::move(message);
        } else {
            fast_path_error_ = "invalid persistent JIT startup response: " + ready;
        }
        stop();
        return false;
    }

    NativeResult server_terminated_result(std::string_view context) {
        NativeResult result;
        result.stdout_text = read_file(files_.stdout_file());
        result.stderr_text = read_file(files_.stderr_file());

        int wait_status = 0;
        if (input_fd_ >= 0) {
            ::close(input_fd_);
            input_fd_ = -1;
        }
        if (output_fd_ >= 0) {
            ::close(output_fd_);
            output_fd_ = -1;
        }
        if (pid_ > 0) {
            while (::waitpid(pid_, &wait_status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
            result.status = native::system_status(wait_status);
        } else {
            result.status = 1;
        }
        if (result.status == 0) result.status = 1;
        if (result.stderr_text.empty())
            result.stderr_text =
                "quidra: " + std::string(context) + "\n";
        return result;
    }

    NativeResult protocol_failure(std::string_view message) {
        stop();
        disabled_ = true;
        NativeResult result;
        result.status = 1;
        result.stdout_text = read_file(files_.stdout_file());
        result.stderr_text =
            "quidra: " + std::string(message) + "\n";
        return result;
    }

    void stop() {
        if (input_fd_ >= 0) {
            ::close(input_fd_);
            input_fd_ = -1;
        }
        if (output_fd_ >= 0) {
            ::close(output_fd_);
            output_fd_ = -1;
        }
        if (pid_ > 0) {
            int wait_status = 0;
            while (::waitpid(pid_, &wait_status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
    }

    const ReplFiles& files_;
    pid_t pid_{-1};
    int input_fd_{-1};
    int output_fd_{-1};
    bool disabled_{};
    std::string fast_path_error_;
};
#else
class PersistentReplJit {
public:
    explicit PersistentReplJit(const ReplFiles& files) : files_(files) {}
    NativeResult run(const Compilation& compilation) {
        return run_external_jit(compilation, files_);
    }
private:
    const ReplFiles& files_;
};
#endif

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
            std::is_same_v<T, ir::CliArgumentOptional> ||
            std::is_same_v<T, ir::CliOption> ||
            std::is_same_v<T, ir::CliFlag> ||
            std::is_same_v<T, ir::CliFinish> ||
            std::is_same_v<T, ir::IoFlush> ||
            std::is_same_v<T, ir::FileOpen> ||
            std::is_same_v<T, ir::FileCreate> ||
            std::is_same_v<T, ir::FileAppend> ||
            std::is_same_v<T, ir::FileHandleRead> ||
            std::is_same_v<T, ir::FileHandleReadLine> ||
            std::is_same_v<T, ir::FileHandleReadBin> ||
            std::is_same_v<T, ir::FileHandleWrite> ||
            std::is_same_v<T, ir::FileHandleFlush> ||
            std::is_same_v<T, ir::FileHandleSeek> ||
            std::is_same_v<T, ir::FileHandleClose> ||
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
            std::is_same_v<T, ir::TaskAll> ||
            std::is_same_v<T, ir::RandomGenerator> ||
            std::is_same_v<T, ir::RandomInt> ||
            std::is_same_v<T, ir::RandomFloat> ||
            std::is_same_v<T, ir::RandomBool> ||
            std::is_same_v<T, ir::ProcessRun> ||
            std::is_same_v<T, ir::ProcessShell> ||
            std::is_same_v<T, ir::HttpGet> ||
            std::is_same_v<T, ir::VideoOpen> ||
            std::is_same_v<T, ir::VideoRead> ||
            std::is_same_v<T, ir::VideoSeek> ||
            std::is_same_v<T, ir::ImageRead> ||
            std::is_same_v<T, ir::ImageWrite> ||
            std::is_same_v<T, ir::NeuralSave> ||
            std::is_same_v<T, ir::NeuralLoad>;
    }, instruction);
}

bool module_requires_replay_barrier(const ir::Module& module) {
    struct ReplayConstant {
        enum class Kind { Boolean, String };
        Kind kind{Kind::Boolean};
        bool boolean{};
        std::string text;
    };

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
        if (function->blocks.empty()) continue;

        std::unordered_map<std::string, const ir::Block*> blocks;
        for (const auto& block : function->blocks)
            blocks.emplace(block.label, &block);

        std::unordered_map<ir::ValueId, ReplayConstant> constants;
        std::vector<const ir::Block*> pending_blocks{&function->blocks.front()};
        std::unordered_set<std::string> visited_blocks;

        while (!pending_blocks.empty()) {
            const auto* block = pending_blocks.back();
            pending_blocks.pop_back();
            if (!visited_blocks.insert(block->label).second) continue;

            for (const auto& instruction : block->instructions) {
                if (const auto* bool_value =
                        std::get_if<ir::ConstantBool>(&instruction)) {
                    constants[bool_value->out] = ReplayConstant{
                        ReplayConstant::Kind::Boolean, bool_value->value, {}};
                } else if (const auto* string_value =
                               std::get_if<ir::ConstantString>(&instruction)) {
                    constants[string_value->out] = ReplayConstant{
                        ReplayConstant::Kind::String, false, string_value->value};
                } else if (const auto* binary =
                               std::get_if<ir::Binary>(&instruction)) {
                    const auto left = constants.find(binary->left);
                    const auto right = constants.find(binary->right);
                    if (left != constants.end() && right != constants.end() &&
                        left->second.kind == ReplayConstant::Kind::String &&
                        right->second.kind == ReplayConstant::Kind::String &&
                        (binary->op == "==" || binary->op == "!=")) {
                        const bool equal =
                            left->second.text == right->second.text;
                        constants[binary->out] = ReplayConstant{
                            ReplayConstant::Kind::Boolean,
                            binary->op == "==" ? equal : !equal, {}};
                    }
                }

                if (replay_barrier_instruction(instruction)) return true;

                if (const auto* call = std::get_if<ir::Call>(&instruction)) {
                    const auto target = functions.find(call->callee);
                    if (target == functions.end()) return true;
                    pending.push_back(target->second);
                }

                // Capture-free function values may hide the dynamic callee from
                // this call-graph walk. Until effect summaries are carried
                // through function values, keep this conservative.
                if (std::holds_alternative<ir::IndirectCall>(instruction))
                    return true;

                if (const auto* jump = std::get_if<ir::Jump>(&instruction)) {
                    const auto target = blocks.find(jump->target);
                    if (target == blocks.end()) return true;
                    pending_blocks.push_back(target->second);
                    break;
                }

                if (const auto* branch =
                        std::get_if<ir::Branch>(&instruction)) {
                    const auto known = constants.find(branch->condition);
                    if (known != constants.end() &&
                        known->second.kind == ReplayConstant::Kind::Boolean) {
                        const auto& label = known->second.boolean
                            ? branch->if_true : branch->if_false;
                        const auto target = blocks.find(label);
                        if (target == blocks.end()) return true;
                        pending_blocks.push_back(target->second);
                    } else {
                        const auto yes = blocks.find(branch->if_true);
                        const auto no = blocks.find(branch->if_false);
                        if (yes == blocks.end() || no == blocks.end()) return true;
                        pending_blocks.push_back(yes->second);
                        pending_blocks.push_back(no->second);
                    }
                    break;
                }

                if (std::holds_alternative<ir::Return>(instruction) ||
                    std::holds_alternative<ir::ReturnVoid>(instruction)) {
                    break;
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
    ReplSession()
        : working_directory_(fs::current_path()),
          jit_(files_) {}

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

        try {
            if (declaration_only) {
                // Declarations/imports change compile-time state only. Validate the complete
                // candidate through the ordinary file-aware checker, then retain the source
                // without LLVM generation, native linking, or process execution.
                (void)check_file_source(files_.source(), candidate, {}, working_directory_);
                accepted_source_ = std::move(candidate);
                return true;
            }

            auto compiled = compile_repl_file_source(
                files_.source(), candidate, {}, working_directory_, replay_prefix_bytes);
            const bool candidate_replay_barrier =
                module_requires_replay_barrier(compiled.compilation.ir);
            // Once JIT execution can reach an external or nondeterministic
            // effect, a failing submission is no longer safely retryable: the
            // effect may have happened before the runtime error. Arm the barrier
            // before launch and keep it even when the candidate is not accepted.
            if (candidate_replay_barrier) replay_barrier_ = true;

            auto native = jit_.run(compiled.compilation);
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

        try {
            auto checked = check_file_source(
                files_.source(), candidate, {}, working_directory_);
            if (checked.program.statements.empty()) {
                std::cerr << "quidra: :type requires an expression\n";
                return;
            }
            const auto& last = checked.program.statements.back();
            const auto* expression_statement = std::get_if<ExprStmt>(&last->data);
            if (!expression_statement) {
                std::cerr << "quidra: :type requires an expression\n";
                return;
            }
            const auto found = checked.expr_types.find(expression_statement->value.get());
            if (found == checked.expr_types.end()) {
                throw std::logic_error("checked REPL expression has no type");
            }
            std::cout << type_name(found->second) << "\n";
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
    PersistentReplJit jit_;
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

#ifdef _WIN32
    const bool interactive_input = _isatty(_fileno(stdin)) != 0;
#else
    const bool interactive_input = ::isatty(STDIN_FILENO) != 0;
#endif

    // A redirected source file is one compilation unit, not a sequence of
    // interactive submissions. Preserve file-parser semantics for `quidra repl
    // < FILE.qui`: internal blank lines, cli bindings, and effects followed by
    // later statements must behave exactly as they do in the source compiler.
    //
    // Redirected command scripts still use the ordinary incremental REPL path.
    std::istringstream buffered_input;
    std::istream* input = &std::cin;
    if (!interactive_input) {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        const auto source = buffer.str();

        bool has_repl_command = false;
        std::istringstream lines(source);
        std::string candidate_line;
        while (std::getline(lines, candidate_line)) {
            if (!candidate_line.empty() && candidate_line.front() == ':') {
                has_repl_command = true;
                break;
            }
        }

        if (!has_repl_command) {
            if (source.find_first_not_of(" \t\r\n") != std::string::npos)
                session.submit(source);
            std::cout << "\n";
            restore_signal();
            return 0;
        }

        buffered_input.str(source);
        input = &buffered_input;
    }

    std::string submission;
    bool block = false;

    while (true) {
        repl_interrupted = 0;
        std::cout << (submission.empty() ? ">>> " : "... ") << std::flush;

        std::string line;
        if (!std::getline(*input, line)) {
            if (interactive_input && repl_interrupted) {
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
