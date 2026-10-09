// How the REPL cuts its input into submissions and decides how to compile
// them; see repl_submission.hpp.
#include "repl_submission.hpp"

#include "quidra/diagnostic.hpp"
#include "quidra/ir.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::cli {
namespace {

bool starts_with_word(const std::string& text, std::string_view word) {
    return text == word ||
           (text.size() > word.size() && text.compare(0, word.size(), word) == 0 &&
            text[word.size()] == ' ');
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
            std::is_same_v<T, ir::Flush> ||
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
            std::is_same_v<T, ir::HttpGet>;
    }, instruction);
}

std::string trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) ++first;
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
    return std::string(text.substr(first, last - first));
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

} // namespace

std::optional<std::string> SubmissionAssembler::add_line(const std::string& line) {
    // A blank line with nothing pending carries no code. Submitting it would
    // append an empty line to the accepted source and re-run the whole
    // accumulated session, repeating every side effect already produced.
    if (submission_.empty() && trim(line).empty()) return std::nullopt;

    if (submission_.empty()) block_ = block_header(line);

    if (block_ && line.empty()) {
        if (!lexically_incomplete(submission_) && !submission_.empty()) {
            block_ = false;
            return std::exchange(submission_, std::string());
        }
        return std::nullopt;
    }

    submission_ += line;
    submission_ += '\n';

    if (!block_ && !lexically_incomplete(submission_)) {
        return std::exchange(submission_, std::string());
    }
    return std::nullopt;
}

void SubmissionAssembler::discard() {
    submission_.clear();
    block_ = false;
}

bool has_repl_command(std::string_view source) {
    std::istringstream lines{std::string(source)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.front() == ':') return true;
    }
    return false;
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

} // namespace quidra::cli
