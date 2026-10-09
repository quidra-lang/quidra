// Call contracts of replacements (call_contract.hpp).
#include "optimizer/call_contract.hpp"

#include "optimizer/rule_registry.hpp"

#include <algorithm>
#include <unordered_set>

namespace quidra::optimizer {

using ir::Parameter;

bool chain_replacement_keeps_safety_traits(
    const std::vector<std::string>& operations, const std::string& replacement_traits,
    const std::unordered_map<std::string, std::string>& operation_traits) {
    std::unordered_set<std::string> required_safety_traits;
    for (const auto& operation : operations) {
        const auto source_traits = operation_traits.find(operation);
        if (source_traits == operation_traits.end()) return false;
        for (const auto& trait :
             descriptor_traits(source_traits->second)) {
            if (compiler_safety_trait(trait))
                required_safety_traits.insert(trait);
        }
    }
    for (const auto& trait : required_safety_traits) {
        if (!descriptor_trait(replacement_traits, trait)) return false;
    }
    return true;
}

bool replacement_keeps_safety_traits(const std::string& source_traits,
                                     const std::string& replacement_traits) {
    for (const auto& trait :
         descriptor_traits(source_traits)) {
        if (compiler_safety_trait(trait) &&
            !descriptor_trait(
                replacement_traits,
                trait)) {
            return false;
        }
    }
    return true;
}

bool same_parameter_contract(const Parameter& replacement, const Parameter& original) {
    // A replacement may require strictly less authority than the
    // original call. In particular, an implicit method receiver is a
    // non-writable borrowed value but is not source-spelled const;
    // forwarding it to a package replacement's `const T &` side input
    // is safe. The reverse direction (dropping const) remains illegal.
    const bool const_compatible =
        replacement.is_const || !original.is_const;
    return replacement.type == original.type &&
           replacement.writable == original.writable &&
           replacement.borrowed == original.borrowed &&
           const_compatible;
}

bool replacement_result_compatible(const Type& declared, const Type& refined) {
    if (declared == refined) return true;
    if (declared.kind != TypeKind::Tensor ||
        refined.kind != TypeKind::Tensor ||
        !declared.first || !refined.first ||
        *declared.first != *refined.first) {
        return false;
    }
    if (declared.length >= 0 && refined.length >= 0 &&
        declared.length != refined.length) {
        return false;
    }
    const auto known = std::min(
        declared.tensor_known_shape_prefix.size(),
        refined.tensor_known_shape_prefix.size());
    for (std::size_t axis = 0; axis < known; ++axis) {
        if (declared.tensor_known_shape_prefix[axis] !=
            refined.tensor_known_shape_prefix[axis]) {
            return false;
        }
    }
    return true;
}

} // namespace quidra::optimizer
