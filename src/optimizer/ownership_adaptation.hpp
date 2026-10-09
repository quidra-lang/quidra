#pragma once

// OwnershipAdaptation: how the replacement a conditional rule names may take
// the call's arguments when its parameters differ from the original
// callee's in ownership only, and the edits that keep every argument's
// lifetime.
//
// Borrowing is inferred from each function body; it is not part of a
// package operation's source signature, so a replacement may differ from
// the original in it. A replacement that borrows what the original consumed
// leaves the caller owning the argument, which is released after the call.
// One that consumes what the original borrowed gets a clone of its own
// (allows_ownership_clone). And a memory-reuse replacement may consume the
// owned temporary that is the call's tensor argument at its last use, taking
// over the release that followed the call (allows_ownership_transfer).
// Any other difference rules the candidate out.

#include "optimizer/function_index.hpp"
#include "optimizer/rule_registry.hpp"
#include "optimizer/rule_stage.hpp"
#include "optimizer/static_tensor_facts.hpp"

#include "quidra/ir/module.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quidra::optimizer {

// Whether a rule of this stage may give a by-value replacement a clone of a
// borrowed argument.
bool allows_ownership_clone(RuleStage stage);

// Whether a rule of this stage may let a replacement take over the release
// of an owned temporary.
bool allows_ownership_transfer(RuleStage stage);

struct OwnershipAdaptation {
    // The replacement function.
    const ir::Function* target{};
    // The index, in the call's block, of the release the target takes over.
    std::optional<std::size_t> release_to_transfer;
    // The arguments the target borrows and the original consumed, with
    // their types: released after the call.
    std::vector<std::pair<ir::ValueId, Type>> borrowed_releases;
    // The indices of the arguments the target consumes and the original
    // borrowed, with their types: cloned before the call.
    std::vector<std::pair<std::size_t, Type>> owned_clones;

    // The adaptation to the one candidate of the rule's replacement that
    // fits the call `instruction` of block: the arity of source_definition,
    // a compatible result, and every parameter as the original's or adapted
    // as above. tensor_argument is the index of the call's first tensor
    // argument, fact the static facts of its value. None unless exactly one
    // candidate fits, or when the one that fits is the callee itself.
    static std::optional<OwnershipAdaptation> select(
        const ConditionalRule& rule, const ir::Function& source_definition,
        std::size_t tensor_argument, const StaticTensorFacts& fact, const ir::Block& block,
        const ir::Instruction& instruction, const std::vector<std::string>& candidates,
        const FunctionIndex& functions);

    // Makes the call, the instruction `instruction` of block in function, a
    // call of target: its clones before it, taking the values after the
    // largest one of the function; the release it takes over erased; its
    // borrowed releases after it. False, with nothing changed, when the
    // function has no value numbers left for the clones.
    bool apply(ir::Function& function, ir::Block& block, ir::Instruction& instruction) const;
};

} // namespace quidra::optimizer
