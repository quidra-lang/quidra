// ConditionalRulePass (conditional_rules.hpp): for each function, block and
// call in order, the execution policies the call observes
// (ExecutionPolicyTracker) and the facts of its first tensor argument; then
// for each rule of the call's operation, in order, RuleConstraints::match,
// the replacement's safety traits, OwnershipAdaptation::select and apply.
// The first rule that applies ends the pass.
#include "optimizer/conditional_rules.hpp"

#include "optimizer/call_contract.hpp"
#include "optimizer/execution_policy.hpp"
#include "optimizer/ownership_adaptation.hpp"
#include "optimizer/static_tensor_facts.hpp"
#include "optimizer/textual_uses.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace quidra::optimizer {

using ir::Call;
using ir::Function;
using ir::Instruction;
using ir::Module;
using ir::ValueId;

bool RuleConstraints::match(const std::string& extension,
                            const std::unordered_map<std::string, std::string>& active_policies,
                            const StaticTensorFacts& fact, const Function& function,
                            const Instruction& call, ValueId value) const {
    if (policy) {
        const auto selected =
            active_policies.find(
                extension);
        if (selected ==
                active_policies.end() ||
            selected->second != *policy) {
            return false;
        }
    }
    if (dtype &&
        (!fact.has_type || !fact.type.first ||
         type_name(*fact.type.first) != *dtype)) {
        return false;
    }
    if (rank &&
        (!fact.has_type ||
         fact.type.length != *rank)) {
        return false;
    }
    if (!shape.empty()) {
        if (!fact.has_type ||
            fact.type.length !=
                static_cast<long long>(
                    shape.size()) ||
            fact.type.tensor_known_shape_prefix.size() <
                shape.size()) {
            return false;
        }
        for (std::size_t axis = 0;
             axis < shape.size(); ++axis) {
            if (fact.type
                    .tensor_known_shape_prefix[axis] !=
                shape[axis]) {
                return false;
            }
        }
    }
    if (device &&
        fact.device != *device) {
        return false;
    }
    if (contiguous.has_value() &&
        (!fact.contiguous ||
         *fact.contiguous != *contiguous)) {
        return false;
    }
    if (tracked.has_value() &&
        (!fact.tracked ||
         *fact.tracked != *tracked)) {
        return false;
    }
    if (owned.has_value() &&
        fact.owns_storage != *owned) {
        return false;
    }
    if (last_use.has_value()) {
        const bool last =
            !has_later_or_cross_block_use(
                function, &call, value);
        if (last != *last_use) return false;
    }
    return true;
}

bool ConditionalRulePass::run(Module& module, const PassContext& context) {
    const auto& bindings = context.registry.bindings();
    const auto& conditional_rules = context.registry.conditional_rules();
    const auto& operation_functions = bindings.operation_functions;
    const auto& operation_traits = bindings.operation_traits;
    if (conditional_rules.empty()) return false;
    for (auto& function : module.functions) {
        const auto facts = StaticTensorFactsAnalysis::run(function);
        for (auto& block : function.blocks) {
            ExecutionPolicyTracker policies(bindings.execution_policy_setters);
            for (auto& instruction : block.instructions) {
                auto* call = std::get_if<Call>(&instruction);
                if (!call) continue;
                if (policies.record_setter(*call)) continue;
                const auto active_execution_policies = policies.take_policies();

                const auto* source_definition =
                    context.functions.find(call->callee);
                if (!source_definition ||
                    source_definition->parameters.size() !=
                        call->args.size()) {
                    continue;
                }

                std::size_t tensor_argument =
                    source_definition->parameters.size();
                for (std::size_t index = 0;
                     index < source_definition->parameters.size();
                     ++index) {
                    if (source_definition->parameters[index].type.kind ==
                            TypeKind::Tensor &&
                        call->args[index].value != 0) {
                        tensor_argument = index;
                        break;
                    }
                }
                if (tensor_argument ==
                    source_definition->parameters.size()) {
                    continue;
                }

                const auto value =
                    call->args[tensor_argument].value;
                StaticTensorFacts fact;
                if (const auto found = facts.find(value);
                    found != facts.end()) {
                    fact = found->second;
                }
                if (!fact.has_type) {
                    fact.type =
                        source_definition->parameters[
                            tensor_argument].type;
                    fact.has_type =
                        fact.type.kind == TypeKind::Tensor;
                }

                for (const auto& rule : conditional_rules) {
                    if (!bindings.call_has_operation(*call, rule.operation))
                        continue;
                    if (!rule.constraints.match(rule.extension, active_execution_policies,
                                                fact, function, instruction, value)) {
                        continue;
                    }

                    const auto replacement_functions =
                        operation_functions.find(rule.replacement);
                    if (replacement_functions ==
                            operation_functions.end() ||
                        replacement_functions->second.empty()) {
                        continue;
                    }
                    const auto source_traits =
                        operation_traits.find(rule.operation);
                    const auto replacement_traits =
                        operation_traits.find(rule.replacement);
                    if (source_traits == operation_traits.end() ||
                        replacement_traits ==
                            operation_traits.end()) {
                        continue;
                    }
                    if (!replacement_keeps_safety_traits(source_traits->second,
                                                         replacement_traits->second)) {
                        continue;
                    }

                    const auto adaptation = OwnershipAdaptation::select(
                        rule, *source_definition, tensor_argument, fact, block, instruction,
                        replacement_functions->second, context.functions);
                    if (!adaptation || !adaptation->apply(function, block, instruction)) continue;
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace quidra::optimizer
