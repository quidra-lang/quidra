#pragma once

// ConditionalRulePass: replaces the callee of a package operation call with
// the replacement a conditional rule names, when the call meets the rule's
// constraints (rule_registry.hpp, ConditionalRule).
//
// The constraints (RuleConstraints) are checked against the static facts of
// the call's first tensor argument (static_tensor_facts.hpp) and the
// execution policies a package setter established earlier in the same block
// (execution_policy.hpp). The replacement must keep the source operation's
// safety traits and be the only function of its operation whose contract
// fits, its parameters differing from the original's in ownership at most
// (ownership_adaptation.hpp). Rules are tried by stage, then by reference.

#include "optimizer/pass_context.hpp"

#include "quidra/ir/module.hpp"

namespace quidra::optimizer {

class ConditionalRulePass {
public:
    // The pass: applies the first rule that matches a call (functions,
    // blocks and calls in order, then rules in order); true when it did,
    // false when none does. Chain fusion runs again after each rewrite
    // (pipeline.hpp), so a rewritten call may join a chain.
    static bool run(ir::Module& module, const PassContext& context);
};

} // namespace quidra::optimizer
