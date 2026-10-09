// OwnershipAdaptation (ownership_adaptation.hpp). apply edits in a fixed
// order, which the value numbers and instruction order depend on: the clones
// are numbered in argument order, then the release is erased, the borrowed
// releases are inserted after the call, and the clones before it.
#include "optimizer/ownership_adaptation.hpp"

#include "optimizer/call_contract.hpp"
#include "optimizer/textual_uses.hpp"

#include "ir/function_builder.hpp"

#include <cstddef>
#include <iterator>
#include <limits>

namespace quidra::optimizer {

using ir::Block;
using ir::Call;
using ir::Clone;
using ir::Function;
using ir::Instruction;
using ir::Release;
using ir::ValueId;

bool allows_ownership_clone(RuleStage stage) {
    return stage != RuleStage::MemoryReuse;
}

bool allows_ownership_transfer(RuleStage stage) {
    return stage == RuleStage::MemoryReuse;
}

namespace {

// The index of the first release of value after current in block; none when
// there is none.
std::optional<std::size_t> transferable_release(const Block& block, const Instruction* current,
                                                ValueId value) {
    std::optional<std::size_t> current_index;
    for (std::size_t index = 0; index < block.instructions.size();
         ++index) {
        if (&block.instructions[index] == current) {
            current_index = index;
            break;
        }
    }
    if (!current_index) return std::nullopt;
    for (std::size_t index = *current_index + 1;
         index < block.instructions.size(); ++index) {
        if (const auto* release =
                std::get_if<Release>(&block.instructions[index]);
            release && release->value == value) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<OwnershipAdaptation> OwnershipAdaptation::select(
    const ConditionalRule& rule, const Function& source_definition,
    std::size_t tensor_argument, const StaticTensorFacts& fact, const Block& block,
    const Instruction& instruction, const std::vector<std::string>& candidates,
    const FunctionIndex& functions) {
    const auto& call = std::get<Call>(instruction);
    const auto value = call.args[tensor_argument].value;
    const Function* replacement_target = nullptr;
    std::optional<std::size_t> ownership_release;
    std::vector<std::pair<ValueId, Type>>
        borrowed_argument_releases;
    std::vector<std::pair<std::size_t, Type>>
        owned_argument_clones;
    std::size_t compatible_targets = 0;
    for (const auto& candidate_name : candidates) {
        const auto* candidate =
            functions.find(candidate_name);
        if (!candidate ||
            candidate->parameters.size() !=
                source_definition.parameters.size() ||
            !replacement_result_compatible(
                candidate->result, call.result)) {
            continue;
        }

        bool compatible = true;
        std::optional<std::size_t> candidate_release;
        std::vector<std::pair<ValueId, Type>>
            candidate_borrowed_releases;
        std::vector<std::pair<std::size_t, Type>>
            candidate_owned_clones;
        for (std::size_t index = 0;
             index < candidate->parameters.size();
             ++index) {
            if (same_parameter_contract(
                    candidate->parameters[index],
                    source_definition.parameters[index])) {
                continue;
            }

            const auto& replacement_parameter =
                candidate->parameters[index];
            const auto& source_parameter =
                source_definition.parameters[index];

            // Borrowing is a lowering optimization inferred
            // from each function body, not part of the
            // source-level package operation signature. A
            // replacement may therefore borrow a value that
            // the original callee consumed by value. The
            // already-lowered caller still owns that
            // argument, so preserve its original lifetime by
            // releasing it after the borrowed replacement
            // returns. The reverse direction remains unsafe:
            // a replacement may not consume an argument that
            // the original call only borrowed.
            const bool ownership_relaxation =
                !source_parameter.borrowed &&
                replacement_parameter.borrowed &&
                replacement_parameter.type ==
                    source_parameter.type &&
                replacement_parameter.writable ==
                    source_parameter.writable &&
                (replacement_parameter.is_const ||
                 !source_parameter.is_const) &&
                index < call.args.size() &&
                call.args[index].value != 0;
            if (ownership_relaxation) {
                candidate_borrowed_releases.push_back(
                    {call.args[index].value,
                     source_parameter.type});
                continue;
            }

            // The opposite inferred-borrowing mismatch is
            // also source-signature-compatible. The lowered
            // caller only borrowed the original argument, so
            // give a by-value replacement its own clone to
            // consume and release. This mirrors normal call
            // lowering without changing package semantics.
            const bool ownership_clone =
                allows_ownership_clone(rule.stage) &&
                source_parameter.borrowed &&
                !replacement_parameter.borrowed &&
                replacement_parameter.type ==
                    source_parameter.type &&
                replacement_parameter.writable ==
                    source_parameter.writable &&
                (replacement_parameter.is_const ||
                 !source_parameter.is_const) &&
                requires_value_clone(
                    source_parameter.type) &&
                index < call.args.size() &&
                call.args[index].value != 0 &&
                !call.args[index].writable_address;
            if (ownership_clone) {
                candidate_owned_clones.push_back(
                    {index, source_parameter.type});
                continue;
            }

            // Memory-reuse targets may consume an owned
            // temporary that the original pure call only
            // borrowed. Lowering emits a post-call Release
            // for such a borrowed temporary. Moving that
            // release into the replacement callee converts
            // the already-owned temporary into the target's
            // by-value ownership without cloning or changing
            // source-visible value semantics.
            const bool ownership_transfer =
                allows_ownership_transfer(rule.stage) &&
                index == tensor_argument &&
                rule.constraints.last_use.value_or(false) &&
                rule.constraints.owned.value_or(false) &&
                fact.owns_storage &&
                source_parameter.borrowed &&
                !replacement_parameter.borrowed &&
                replacement_parameter.type ==
                    source_parameter.type &&
                replacement_parameter.writable ==
                    source_parameter.writable &&
                replacement_parameter.is_const ==
                    source_parameter.is_const;
            if (!ownership_transfer) {
                compatible = false;
                break;
            }
            candidate_release =
                transferable_release(
                    block, &instruction, value);
            if (!candidate_release) {
                compatible = false;
                break;
            }
        }
        if (!compatible) continue;
        replacement_target = candidate;
        ownership_release = candidate_release;
        borrowed_argument_releases =
            std::move(candidate_borrowed_releases);
        owned_argument_clones =
            std::move(candidate_owned_clones);
        ++compatible_targets;
    }
    if (compatible_targets != 1 ||
        !replacement_target ||
        replacement_target->name == call.callee) {
        return std::nullopt;
    }
    return OwnershipAdaptation{replacement_target, ownership_release,
                               std::move(borrowed_argument_releases),
                               std::move(owned_argument_clones)};
}

bool OwnershipAdaptation::apply(Function& function, Block& block,
                                Instruction& instruction) const {
    auto& call = std::get<Call>(instruction);
    const auto call_index =
        static_cast<std::size_t>(
            &instruction -
            block.instructions.data());
    std::vector<Instruction> argument_clones;
    if (!owned_clones.empty()) {
        const auto first_fresh =
            next_rewrite_value(function);
        if (!first_fresh ||
            owned_clones.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<ValueId>::max() -
                    *first_fresh + 1)) {
            return false;
        }
        ir::FunctionBuilder values(function, *first_fresh, 0, 0);
        argument_clones.reserve(
            owned_clones.size());
        for (const auto& [argument_index, type] :
             owned_clones) {
            const auto original =
                call.args[argument_index].value;
            const auto cloned = values.fresh();
            argument_clones.push_back(
                Clone{cloned, original, type});
            call.args[argument_index].value = cloned;
        }
    }

    call.callee = target->name;
    if (release_to_transfer) {
        block.instructions.erase(
            block.instructions.begin() +
            static_cast<std::ptrdiff_t>(
                *release_to_transfer));
    }
    if (!borrowed_releases.empty()) {
        std::vector<Instruction> releases;
        releases.reserve(
            borrowed_releases.size());
        for (const auto& [argument, type] :
             borrowed_releases) {
            releases.push_back(
                Release{argument, type});
        }
        block.instructions.insert(
            block.instructions.begin() +
                static_cast<std::ptrdiff_t>(
                    call_index + 1),
            std::make_move_iterator(releases.begin()),
            std::make_move_iterator(releases.end()));
    }
    if (!argument_clones.empty()) {
        block.instructions.insert(
            block.instructions.begin() +
                static_cast<std::ptrdiff_t>(
                    call_index),
            std::make_move_iterator(
                argument_clones.begin()),
            std::make_move_iterator(
                argument_clones.end()));
    }
    return true;
}

} // namespace quidra::optimizer
