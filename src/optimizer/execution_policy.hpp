#pragma once

// ExecutionPolicyTracker: the package execution policies known at each call
// of one block. Execution policy is mutable process state, so it is known
// only after a package-declared setter in the same block: a call of a setter
// sets its extension's policy, and every other call observes the policies
// set before it and leaves them unknown, since an opaque callee may change
// process-wide policy. A block starts with none known; policies do not
// cross block or function boundaries.

#include "optimizer/rule_registry.hpp"

#include "quidra/ir/module.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::optimizer {

class ExecutionPolicyTracker {
public:
    explicit ExecutionPolicyTracker(
        const std::unordered_map<std::string, std::vector<ExecutionPolicySetter>>& setters)
        : setters_(setters) {}

    // Whether call runs a declared setter; if it does, records the policies
    // it sets.
    bool record_setter(const ir::Call& call);

    // The policies, by extension identity, that a call that is no setter
    // observes; they are unknown after it. The map is only looked up.
    std::unordered_map<std::string, std::string> take_policies();

private:
    const std::unordered_map<std::string, std::vector<ExecutionPolicySetter>>& setters_;
    std::unordered_map<std::string, std::string> policies_;
};

} // namespace quidra::optimizer
