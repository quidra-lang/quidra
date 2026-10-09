// ChainFusionPass (chain_fusion.hpp): for each function, pattern, block and
// call in order, match_chain finds a chain of the pattern's operations, then
// select_replacement_target picks the function that replaces it and
// eliminated_values_escape checks that only the chain uses its intermediate
// values; the first chain that passes is spliced into its block and the
// releases of its intermediate values are erased.
#include "optimizer/chain_fusion.hpp"

#include "optimizer/call_contract.hpp"
#include "optimizer/textual_uses.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quidra::optimizer {

using ir::Block;
using ir::Call;
using ir::CallArgument;
using ir::Clone;
using ir::FieldGet;
using ir::Function;
using ir::Instruction;
using ir::LoadLocal;
using ir::LoadReference;
using ir::Module;
using ir::Parameter;
using ir::Release;
using ir::Retain;
using ir::SourceLocation;
using ir::ValueId;

namespace {

// A chain of a pattern's operations in one block.
struct ChainMatch {
    // The indices of the chain's calls in the block, first to last.
    std::vector<std::size_t> call_indices;
    // The results of every call but the last: the fusion eliminates them.
    std::vector<ValueId> eliminated_values;
    // The later calls' arguments other than the chained value, in chain
    // order, and the parameters that take them: the replacement's side
    // inputs.
    std::vector<CallArgument> captured_arguments;
    std::vector<Parameter> captured_parameters;
    // Releases between the calls of a first-call argument or an eliminated
    // value, which move after the replacement.
    std::vector<std::size_t> deferred_release_indices;
    std::vector<Release> deferred_releases;
};

bool fusion_bridge_instruction(const Instruction& instruction) {
    return std::holds_alternative<SourceLocation>(instruction) ||
           std::holds_alternative<LoadLocal>(instruction) ||
           std::holds_alternative<LoadReference>(instruction) ||
           std::holds_alternative<FieldGet>(instruction) ||
           std::holds_alternative<Clone>(instruction) ||
           std::holds_alternative<Retain>(instruction);
}

// The chain of pattern's operations that starts with first_call, the
// instruction at index start of block: each later call follows the previous
// one with only bridges and deferrable releases in between, implements the
// next operation, takes the previous result exactly once and no writable
// side input, and names a definition of its arity. None when a call does not
// follow.
std::optional<ChainMatch> match_chain(const Block& block, std::size_t start,
                                      const Call& first_call, const FusionPattern& pattern,
                                      const PassContext& context) {
    const auto& bindings = context.registry.bindings();
    std::vector<std::size_t> call_indices{start};
    std::vector<ValueId> eliminated_values;
    std::vector<CallArgument> captured_arguments;
    std::vector<Parameter> captured_parameters;
    std::vector<std::size_t> deferred_release_indices;
    std::vector<Release> deferred_releases;
    std::unordered_set<ValueId> first_argument_values;
    for (const auto& argument : first_call.args) {
        if (argument.value != 0)
            first_argument_values.insert(argument.value);
        if (argument.writable_address)
            first_argument_values.insert(
                *argument.writable_address);
    }

    const Call* current = &first_call;
    for (std::size_t position = 1;
         position < pattern.operations.size();
         ++position) {
        std::size_t next = call_indices.back() + 1;
        for (; next < block.instructions.size(); ++next) {
            const auto& bridge =
                block.instructions[next];
            if (const auto* release =
                    std::get_if<Release>(&bridge);
                release &&
                (first_argument_values.contains(
                     release->value) ||
                 std::find(
                     eliminated_values.begin(),
                     eliminated_values.end(),
                     release->value) !=
                     eliminated_values.end())) {
                deferred_release_indices.push_back(next);
                deferred_releases.push_back(*release);
                continue;
            }
            if (!fusion_bridge_instruction(bridge))
                break;
            if (!std::holds_alternative<SourceLocation>(
                    bridge) &&
                instruction_mentions_value(
                    bridge, current->out)) {
                return std::nullopt;
            }
        }
        if (next >= block.instructions.size()) return std::nullopt;

        const auto* next_call =
            std::get_if<Call>(
                &block.instructions[next]);
        if (!next_call ||
            !bindings.call_has_operation(
                *next_call,
                pattern.operations[position])) {
            return std::nullopt;
        }
        const auto* next_definition =
            context.functions.find(next_call->callee);
        if (!next_definition ||
            next_definition->parameters.size() !=
                next_call->args.size()) {
            return std::nullopt;
        }

        std::optional<std::size_t> chain_argument;
        for (std::size_t argument_index = 0;
             argument_index < next_call->args.size();
             ++argument_index) {
            const auto& argument =
                next_call->args[argument_index];
            if (!argument.writable_address &&
                argument.value == current->out) {
                if (chain_argument) return std::nullopt;
                chain_argument = argument_index;
            }
        }
        if (!chain_argument) return std::nullopt;

        for (std::size_t argument_index = 0;
             argument_index < next_call->args.size();
             ++argument_index) {
            if (argument_index == *chain_argument)
                continue;
            const auto& argument =
                next_call->args[argument_index];
            const auto& parameter =
                next_definition->parameters[
                    argument_index];
            if (argument.writable_address ||
                parameter.writable ||
                argument.value == 0) {
                return std::nullopt;
            }
            captured_arguments.push_back(argument);
            captured_parameters.push_back(parameter);
        }

        eliminated_values.push_back(current->out);
        call_indices.push_back(next);
        current = next_call;
    }
    return ChainMatch{std::move(call_indices), std::move(eliminated_values),
                      std::move(captured_arguments), std::move(captured_parameters),
                      std::move(deferred_release_indices), std::move(deferred_releases)};
}

// The function, among the candidates bound to the replacement operation,
// that can stand in for the chain: it takes the side inputs and then the
// first call's arguments, each as the chain's parameters take them, and
// returns a result compatible with the last call's. Null unless exactly one
// candidate fits.
const Function* select_replacement_target(const ChainMatch& match,
                                          const Function& first_definition,
                                          const Call& final_call,
                                          const std::vector<std::string>& candidates,
                                          const FunctionIndex& functions) {
    const Function* replacement_target = nullptr;
    std::size_t compatible_targets = 0;
    for (const auto& candidate_name : candidates) {
        const auto* candidate =
            functions.find(candidate_name);
        if (!candidate ||
            candidate->parameters.size() !=
                match.captured_parameters.size() +
                    first_definition.parameters.size() ||
            !replacement_result_compatible(
                candidate->result,
                final_call.result)) {
            continue;
        }
        bool compatible = true;
        std::size_t parameter_index = 0;
        for (const auto& expected :
             match.captured_parameters) {
            if (!same_parameter_contract(
                    candidate->parameters[
                        parameter_index++],
                    expected)) {
                compatible = false;
                break;
            }
        }
        if (compatible) {
            for (const auto& expected :
                 first_definition.parameters) {
                if (!same_parameter_contract(
                        candidate->parameters[
                            parameter_index++],
                        expected)) {
                    compatible = false;
                    break;
                }
            }
        }
        if (!compatible) continue;
        replacement_target = candidate;
        ++compatible_targets;
    }
    if (compatible_targets != 1 ||
        !replacement_target) {
        return nullptr;
    }
    return replacement_target;
}

// Whether an instruction of function outside the chain, other than a
// release, uses one of the values the fusion eliminates.
bool eliminated_values_escape(const Function& function, const Block& block,
                              const ChainMatch& match) {
    for (const auto value : match.eliminated_values) {
        for (std::size_t block_index = 0;
             block_index < function.blocks.size();
             ++block_index) {
            const auto& inspected_block =
                function.blocks[block_index];
            for (std::size_t instruction_index = 0;
                 instruction_index <
                     inspected_block.instructions.size();
                 ++instruction_index) {
                if (&inspected_block == &block &&
                    std::find(
                        match.call_indices.begin(),
                        match.call_indices.end(),
                        instruction_index) !=
                        match.call_indices.end()) {
                    continue;
                }
                if (const auto* release =
                        std::get_if<Release>(
                            &inspected_block.instructions[
                                instruction_index]);
                    release &&
                    release->value == value) {
                    continue;
                }
                if (instruction_mentions_value(
                        inspected_block.instructions[
                            instruction_index],
                        value)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// The call that replaces the chain: the side inputs, then first_call's
// arguments, with the last call's result value, type and source position.
Call replacement_call(const Call& first_call, const Call& final_call,
                      const Function& replacement_target, const ChainMatch& match) {
    Call replacement = first_call;
    replacement.out = final_call.out;
    replacement.callee =
        replacement_target.name;
    replacement.result = final_call.result;
    replacement.line = final_call.line;
    replacement.column = final_call.column;
    replacement.args.clear();
    replacement.args.insert(
        replacement.args.end(),
        match.captured_arguments.begin(),
        match.captured_arguments.end());
    replacement.args.insert(
        replacement.args.end(),
        first_call.args.begin(),
        first_call.args.end());
    return replacement;
}

// Puts replacement where the chain's last call was, followed by the deferred
// releases, and drops the chain's other calls and the deferred releases from
// where they were.
void splice_replacement(Block& block, const ChainMatch& match, Call replacement) {
    const auto final_index = match.call_indices.back();
    std::unordered_set<std::size_t> removed_indices;
    for (std::size_t index = 0;
         index + 1 < match.call_indices.size(); ++index) {
        removed_indices.insert(match.call_indices[index]);
    }
    removed_indices.insert(
        match.deferred_release_indices.begin(),
        match.deferred_release_indices.end());

    std::vector<Instruction> rewritten_instructions;
    rewritten_instructions.reserve(
        block.instructions.size() -
        removed_indices.size() +
        match.deferred_releases.size());
    for (std::size_t instruction_index = 0;
         instruction_index < block.instructions.size();
         ++instruction_index) {
        if (removed_indices.contains(
                instruction_index)) {
            continue;
        }
        if (instruction_index == final_index) {
            rewritten_instructions.push_back(
                std::move(replacement));
            for (const auto& release :
                 match.deferred_releases) {
                rewritten_instructions.push_back(
                    release);
            }
            continue;
        }
        rewritten_instructions.push_back(
            std::move(
                block.instructions[
                    instruction_index]));
    }
    block.instructions =
        std::move(rewritten_instructions);
}

// Erases every release of an eliminated value from function: the values no
// longer exist.
void erase_eliminated_releases(Function& function,
                               const std::vector<ValueId>& eliminated_values) {
    for (auto& cleanup_block :
         function.blocks) {
        cleanup_block.instructions.erase(
            std::remove_if(
                cleanup_block.instructions.begin(),
                cleanup_block.instructions.end(),
                [&](const Instruction& instruction) {
                    const auto* release =
                        std::get_if<Release>(
                            &instruction);
                    return release &&
                        std::find(
                            eliminated_values.begin(),
                            eliminated_values.end(),
                            release->value) !=
                            eliminated_values.end();
                }),
            cleanup_block.instructions.end());
    }
}

// Rewrites the first chain found (functions, then patterns, then blocks
// and calls in order); false when there is none.
bool apply_one(Module& module, const PassContext& context) {
    const auto& bindings = context.registry.bindings();
    const auto& extension_fusions = context.registry.extension_tables().fusions;
    const auto& operation_functions = bindings.operation_functions;
    const auto& operation_traits = bindings.operation_traits;
    for (auto& function : module.functions) {
        for (const auto& [identity, patterns] : extension_fusions) {
            (void)identity;
            for (const auto& pattern : patterns) {
                if (!pattern.replacement ||
                    pattern.operations.size() < 2) {
                    continue;
                }
                const auto replacement_functions =
                    operation_functions.find(*pattern.replacement);
                if (replacement_functions == operation_functions.end() ||
                    replacement_functions->second.empty()) {
                    continue;
                }
                const auto replacement_traits =
                    operation_traits.find(*pattern.replacement);
                if (replacement_traits == operation_traits.end()) continue;
                if (!chain_replacement_keeps_safety_traits(
                        pattern.operations, replacement_traits->second,
                        operation_traits)) {
                    continue;
                }
                // Never rewrite the package-owned fallback implementation
                // into a call to itself.
                if (std::find(
                        replacement_functions->second.begin(),
                        replacement_functions->second.end(),
                        function.name) !=
                    replacement_functions->second.end()) {
                    continue;
                }

                for (auto& block : function.blocks) {
                    for (std::size_t start = 0;
                         start < block.instructions.size(); ++start) {
                        auto* first_call =
                            std::get_if<Call>(&block.instructions[start]);
                        if (!first_call ||
                            !bindings.call_has_operation(
                                *first_call,
                                pattern.operations.front())) {
                            continue;
                        }

                        const auto* first_definition =
                            context.functions.find(first_call->callee);
                        if (!first_definition ||
                            first_definition->parameters.size() !=
                                first_call->args.size()) {
                            continue;
                        }

                        const auto match =
                            match_chain(block, start, *first_call, pattern, context);
                        if (!match) continue;

                        const auto final_call =
                            *std::get_if<Call>(
                                &block.instructions[
                                    match->call_indices.back()]);
                        const auto* replacement_target = select_replacement_target(
                            *match, *first_definition, final_call,
                            replacement_functions->second, context.functions);
                        if (!replacement_target) continue;
                        if (eliminated_values_escape(function, block, *match)) continue;

                        auto replacement = replacement_call(
                            *first_call, final_call, *replacement_target, *match);
                        splice_replacement(block, *match, std::move(replacement));
                        erase_eliminated_releases(function, match->eliminated_values);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace

// A package may name an operation-id replacement for a fusion pattern.
// Core interprets only generic SSA/call contracts: it never assigns domain
// meaning to operation ids. A same-block chain may carry immutable side
// inputs on later calls. Those side inputs are captured in chain order and
// prepended to the first call's original arguments. The replacement is
// emitted at the final call site so side-input evaluation order is preserved.
// Only a narrow set of pure value-producing bridge instructions may appear
// between calls, and writable side inputs are deliberately excluded.
bool ChainFusionPass::run(Module& module, const PassContext& context) {
    bool rewritten = false;
    while (apply_one(module, context)) rewritten = true;
    return rewritten;
}

} // namespace quidra::optimizer
