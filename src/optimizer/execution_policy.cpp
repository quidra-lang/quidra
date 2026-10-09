// ExecutionPolicyTracker (execution_policy.hpp).
#include "optimizer/execution_policy.hpp"

#include <utility>

namespace quidra::optimizer {

bool ExecutionPolicyTracker::record_setter(const ir::Call& call) {
    const auto setters = setters_.find(call.callee);
    if (setters == setters_.end()) return false;
    for (const auto& setter : setters->second)
        policies_[setter.extension] = setter.value;
    return true;
}

std::unordered_map<std::string, std::string> ExecutionPolicyTracker::take_policies() {
    return std::exchange(policies_, {});
}

} // namespace quidra::optimizer
