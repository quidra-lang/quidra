// FiniteCheckMerging (finite_check_merging.hpp).
#include "optimizer/finite_check_merging.hpp"

#include "quidra/ir/visit.hpp"
#include "quidra/ir/value_allocator.hpp"
#include "quidra/ir/check.hpp"
#include "quidra/ir/numeric.hpp"
#include "quidra/numeric_types.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::optimizer {
namespace {

using ir::Block;
using ir::CheckedReal;
using ir::FiniteCause;
using ir::FiniteCheck;
using ir::Function;
using ir::Instruction;
using ir::ValueId;

// The semantic SSA reads, independent of the diagnostic renderer.
std::vector<ValueId> read_values(const Instruction& instruction) {
    struct Reads final : ir::OperandVisitor {
        void use(ValueId value) override { values.push_back(value); }
        std::vector<ValueId> values;
    } visitor;
    ir::visit_operands(instruction, visitor);
    return visitor.values;
}

bool is_finite_instruction(const Instruction& instruction) {
    return std::holds_alternative<FiniteCheck>(instruction) ||
           std::holds_alternative<ir::FiniteFlagUpdate>(instruction) ||
           std::holds_alternative<ir::FiniteFlagCheck>(instruction);
}

bool is_real_arithmetic(const ir::Binary& binary) {
    return is_fixed_real(binary.operand_type) &&
           (binary.op == "+" || binary.op == "-" || binary.op == "*" || binary.op == "/");
}

bool is_comparison(std::string_view op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

// A real conversion that only widens (real32 -> real64): it neither fails
// nor rounds.
bool is_real_widening(const ir::NumericConvert& convert) {
    if (convert.checked_range || !is_fixed_real(convert.source_type) ||
        !is_fixed_real(convert.target_type))
        return false;
    const auto* from = numeric_kind_info(convert.source_type.kind);
    const auto* to = numeric_kind_info(convert.target_type.kind);
    return from && to && ieee_precision_bits(from->format) <= ieee_precision_bits(to->format) &&
           from->width <= to->width;
}

// An instruction that may stand between merged checks: it neither fails nor
// has an effect.
bool quiet_in_expression(const Instruction& instruction) {
    if (std::holds_alternative<ir::ConstantInt>(instruction) ||
        std::holds_alternative<ir::ConstantFloat>(instruction) ||
        std::holds_alternative<ir::ConstantBool>(instruction) ||
        std::holds_alternative<ir::LoadLocal>(instruction))
        return true;
    if (const auto* binary = std::get_if<ir::Binary>(&instruction)) {
        if (is_real_arithmetic(*binary)) return true;
        return is_comparison(binary->op) &&
               (is_fixed_real(binary->operand_type) || is_fixed_integer(binary->operand_type) ||
                binary->operand_type.kind == TypeKind::Bool);
    }
    if (const auto* unary = std::get_if<ir::Unary>(&instruction))
        return unary->op == "-" && is_fixed_real(unary->type);
    if (const auto* convert = std::get_if<ir::NumericConvert>(&instruction))
        return is_real_widening(*convert);
    return false;
}

// An instruction that may stand in a loop whose check is sunk: it neither
// fails nor has an effect the program can observe before the loop ends.
bool quiet_in_loop(const Instruction& instruction) {
    if (quiet_in_expression(instruction)) return true;
    if (std::holds_alternative<ir::Jump>(instruction) ||
        std::holds_alternative<ir::Branch>(instruction) ||
        std::holds_alternative<ir::ArrayLength>(instruction))
        return true;
    const auto scalar = [](const Type& type) {
        return is_numeric(type) || type.kind == TypeKind::Bool;
    };
    if (const auto* store = std::get_if<ir::StoreLocal>(&instruction))
        return scalar(store->type);
    if (const auto* binary = std::get_if<ir::Binary>(&instruction)) {
        if (binary->operand_type.kind == TypeKind::Bool)
            return binary->op == "and" || binary->op == "or";
        return is_fixed_integer(binary->operand_type) && binary->overflow_proven &&
               (binary->op == "+" || binary->op == "-" || binary->op == "*");
    }
    if (const auto* unary = std::get_if<ir::Unary>(&instruction))
        return unary->op == "not" && unary->type.kind == TypeKind::Bool;
    if (const auto* convert = std::get_if<ir::NumericConvert>(&instruction))
        return convert->inline_proven && !convert->checked_range;
    if (const auto* get = std::get_if<ir::ArrayGet>(&instruction))
        return get->bounds_proven && get->initialization_proven && !get->bounds_guard &&
               !get->initialization_guard && scalar(get->element_type);
    if (const auto* set = std::get_if<ir::ArraySet>(&instruction))
        return set->bounds_proven && set->initialization_proven && !set->bounds_guard &&
               !set->initialization_guard && scalar(set->element_type);
    return false;
}

// How a value is used: as an operand that keeps a non-finite operand
// non-finite, of the operation computing `consumer`, or otherwise.
struct Use {
    bool propagating{};
    ValueId consumer{};
};

struct CheckSite {
    std::size_t index{};
    CheckedReal value;
};

// Merges the checks of one block; true when it removed any.
bool merge_block(Block& block, const std::unordered_map<ValueId, std::vector<Use>>& uses) {
    std::vector<CheckSite> checks;
    std::unordered_map<ValueId, std::size_t> check_of;
    std::unordered_map<ValueId, std::size_t> defined_at;
    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
        const auto& instruction = block.instructions[i];
        if (const auto* check = std::get_if<FiniteCheck>(&instruction);
            check && check->primary.size() == 1 && check->covered.empty()) {
            check_of[check->primary.front().value] = checks.size();
            checks.push_back({i, check->primary.front()});
        } else if (const auto* binary = std::get_if<ir::Binary>(&instruction)) {
            defined_at[binary->out] = i;
        }
    }
    if (checks.size() < 2) return false;

    // parent[c]: the check that takes check c, when every use of c's value
    // propagates into one later checked operation of this block.
    std::vector<std::optional<std::size_t>> parent(checks.size());
    std::vector<std::vector<std::size_t>> children(checks.size());
    for (std::size_t c = 0; c < checks.size(); ++c) {
        const auto found = uses.find(checks[c].value.value);
        if (found == uses.end() || found->second.empty()) continue;
        const auto consumer = found->second.front().consumer;
        const bool single = std::all_of(found->second.begin(), found->second.end(),
                                        [&](const Use& use) {
                                            return use.propagating && use.consumer == consumer;
                                        });
        if (!single) continue;
        const auto definition = defined_at.find(consumer);
        const auto taker = check_of.find(consumer);
        if (definition == defined_at.end() || taker == check_of.end()) continue;
        if (definition->second < checks[c].index ||
            checks[taker->second].index < definition->second)
            continue;
        parent[c] = taker->second;
        children[taker->second].push_back(c);
    }

    std::set<std::size_t> removed;
    std::unordered_map<std::size_t, std::vector<CheckedReal>> covered;
    const auto descendants = [&](std::size_t root) {
        std::vector<std::size_t> all;
        std::vector<std::size_t> pending = children[root];
        while (!pending.empty()) {
            const auto c = pending.back();
            pending.pop_back();
            all.push_back(c);
            pending.insert(pending.end(), children[c].begin(), children[c].end());
        }
        std::sort(all.begin(), all.end());
        return all;
    };
    // Merges the tree under `root` when nothing between its first check
    // and `root` fails or has an effect; otherwise each child's own tree.
    const auto merge = [&](const auto& self, std::size_t root) -> void {
        const auto tree = descendants(root);
        if (tree.empty()) return;
        std::set<std::size_t> positions;
        for (const auto c : tree) positions.insert(checks[c].index);
        bool quiet = true;
        for (auto i = checks[tree.front()].index + 1; quiet && i < checks[root].index; ++i) {
            const auto& instruction = block.instructions[i];
            quiet = std::holds_alternative<FiniteCheck>(instruction) ? positions.contains(i)
                                                                      : quiet_in_expression(instruction);
        }
        if (!quiet) {
            for (const auto child : children[root]) self(self, child);
            return;
        }
        auto& values = covered[root];
        for (const auto c : tree) {
            removed.insert(checks[c].index);
            values.push_back(checks[c].value);
        }
    };
    for (std::size_t c = 0; c < checks.size(); ++c)
        if (!parent[c]) merge(merge, c);
    if (removed.empty()) return false;

    for (auto& [root, values] : covered)
        std::get<FiniteCheck>(block.instructions[checks[root].index]).covered = std::move(values);
    std::vector<Instruction> kept;
    kept.reserve(block.instructions.size() - removed.size());
    for (std::size_t i = 0; i < block.instructions.size(); ++i)
        if (!removed.contains(i)) kept.push_back(std::move(block.instructions[i]));
    block.instructions = std::move(kept);
    return true;
}

std::optional<std::size_t> block_named(const Function& function, const std::string& label) {
    for (std::size_t i = 0; i < function.blocks.size(); ++i)
        if (function.blocks[i].label == label) return i;
    return std::nullopt;
}

// The successors of a block, or nullopt when it ends otherwise (a return).
std::optional<std::vector<std::string>> successors(const Block& block) {
    if (block.instructions.empty()) return std::nullopt;
    const auto& last = block.instructions.back();
    if (const auto* jump = std::get_if<ir::Jump>(&last)) return std::vector{jump->target};
    if (const auto* branch = std::get_if<ir::Branch>(&last))
        return std::vector{branch->if_true, branch->if_false};
    return std::nullopt;
}

// Sinks the check of the range loop whose header is block `header`, when
// it qualifies; true when it did.
bool sink_loop(Function& function, std::size_t header) {
    const auto& label = function.blocks[header].label;
    const bool range = label.starts_with("for.range.cond.");
    const std::string_view prefix = range ? "for.range.cond." : "for.cond.";
    if (!label.starts_with(prefix)) return false;
    std::size_t number = 0;
    for (const auto c : std::string_view(label).substr(prefix.size())) {
        if (c < '0' || c > '9') return false;
        number = number * 10 + static_cast<std::size_t>(c - '0');
    }
    const auto body = block_named(function, std::string(range ? "for.range.body." : "for.body.") +
                                              std::to_string(number + 1));
    const auto step = block_named(function, std::string(range ? "for.range.advance." : "for.step.") +
                                              std::to_string(number + 2));
    const auto end = block_named(function, std::string(range ? "for.range.end." : "for.end.") +
                                             std::to_string(number + 3));
    if (!body || !step || !end) return false;
    const auto* condition = function.blocks[header].instructions.empty()
                                ? nullptr
                                : std::get_if<ir::Branch>(&function.blocks[header].instructions.back());
    if (!condition || condition->if_true != function.blocks[*body].label ||
        condition->if_false != function.blocks[*end].label)
        return false;

    // The loop's blocks: those the body reaches without passing the header.
    std::set<std::size_t> loop{*body};
    std::vector<std::size_t> pending{*body};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto next = successors(function.blocks[current]);
        if (!next) return false;
        for (const auto& target : *next) {
            const auto found = block_named(function, target);
            if (!found || *found == *end) return false;
            if (*found == header || loop.contains(*found)) continue;
            loop.insert(*found);
            pending.push_back(*found);
        }
    }
    if (!loop.contains(*step)) return false;

    // One predecessor from outside, which jumps to the header.
    std::optional<std::size_t> preheader;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
        if (i == header || loop.contains(i)) continue;
        const auto next = successors(function.blocks[i]);
        if (!next || std::find(next->begin(), next->end(), label) == next->end()) continue;
        if (preheader || next->size() != 1) return false;
        preheader = i;
    }
    if (!preheader) return false;
    // The exit is reached from the header only.
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
        if (i == header) continue;
        const auto next = successors(function.blocks[i]);
        if (next && std::find(next->begin(), next->end(), function.blocks[*end].label) != next->end())
            return false;
    }

    // Nothing fails or has an effect but one check of one operation with a
    // single cause, under one statement.
    std::optional<std::pair<std::size_t, std::size_t>> check_at;
    std::optional<std::uint64_t> statement;
    std::optional<std::pair<std::size_t, std::size_t>> statement_at;
    std::vector<std::size_t> blocks{header};
    blocks.insert(blocks.end(), loop.begin(), loop.end());
    for (const auto b : blocks) {
        const auto& instructions = function.blocks[b].instructions;
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            const auto& instruction = instructions[i];
            if (const auto* check = std::get_if<FiniteCheck>(&instruction)) {
                if (check_at || check->primary.size() != 1 || !check->covered.empty()) return false;
                const auto cause = check->primary.front().cause;
                if (cause != FiniteCause::addition && cause != FiniteCause::subtraction &&
                    cause != FiniteCause::multiplication)
                    return false;
                check_at = std::pair{b, i};
            } else if (const auto* boundary = std::get_if<ir::StatementBoundary>(&instruction)) {
                if (b == header || boundary->identity == 0) return false;
                if (statement) {
                    if (*statement != boundary->identity) return false;
                } else {
                    statement = boundary->identity;
                    statement_at = std::pair{b, i};
                }
            } else if (std::holds_alternative<ir::SourceLocation>(instruction)) {
                // An imported/hand-built source marker needs a semantic site.
                // Its coordinates and attribution fields are never inspected.
                if (!statement) return false;
            } else if (is_finite_instruction(instruction) || !quiet_in_loop(instruction)) {
                return false;
            }
        }
    }
    if (!check_at) return false;
    // The statement that is current at the check is current at the exit.
    if (statement && (statement_at->first != check_at->first ||
                      statement_at->second > check_at->second))
        return false;

    const auto cleared = ir::allocate_value(function);
    if (!cleared) return false;
    auto& sunk = function.blocks[check_at->first].instructions[check_at->second];
    const auto value = std::get<FiniteCheck>(sunk).primary.front();
    const auto flag = "$finite.flag." + std::to_string(value.value);
    sunk = ir::FiniteFlagUpdate{flag, value};
    auto& entry = function.blocks[*preheader].instructions;
    entry.insert(entry.end() - 1, ir::ConstantBool{*cleared, false});
    entry.insert(entry.end() - 1, ir::StoreLocal{flag, *cleared, Type::simple(TypeKind::Bool)});
    auto& exit = function.blocks[*end].instructions;
    exit.insert(exit.begin(), ir::FiniteFlagCheck{flag, value});
    return true;
}

bool run_function(Function& function) {
    bool changed = false;
    std::unordered_map<ValueId, std::vector<Use>> uses;
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions) {
            if (is_finite_instruction(instruction)) continue;
            if (const auto* binary = std::get_if<ir::Binary>(&instruction);
                binary && is_real_arithmetic(*binary)) {
                uses[binary->left].push_back({true, binary->out});
                uses[binary->right].push_back({binary->op != "/", binary->out});
                continue;
            }
            for (const auto value : read_values(instruction)) uses[value].push_back({});
        }
    for (auto& block : function.blocks) changed = merge_block(block, uses) || changed;

    bool has_check = false;
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            has_check = has_check || std::holds_alternative<FiniteCheck>(instruction);
    if (!has_check) return changed;
    for (std::size_t header = 0; header < function.blocks.size(); ++header)
        changed = sink_loop(function, header) || changed;
    return changed;
}

} // namespace

bool FiniteCheckMerging::enabled(const PassContext& context) {
    return !context.options.keep_every_finite_check;
}

bool FiniteCheckMerging::run(ir::Module& module, const PassContext&) {
    bool changed = false;
    for (auto& function : module.functions) {
        bool has_check = false;
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                has_check = has_check || std::holds_alternative<FiniteCheck>(instruction);
        if (has_check) changed = run_function(function) || changed;
    }
    return changed;
}

} // namespace quidra::optimizer
