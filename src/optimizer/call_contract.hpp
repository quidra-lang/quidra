#pragma once

// The call contract a replacement function must keep to stand in for the
// function a call names: the safety traits of what it replaces, each
// parameter as the original's (or with less authority), and a result of
// the call's type.

#include "quidra/ir/module.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::optimizer {

// Whether a fusion replacement, whose operation has replacement_traits,
// keeps every safety trait (compiler_safety_trait) of the chain's
// operations. False also when an operation of the chain has no traits:
// then the chain has no complete safety contract. operation_traits maps an
// operation to its traits list.
bool chain_replacement_keeps_safety_traits(
    const std::vector<std::string>& operations, const std::string& replacement_traits,
    const std::unordered_map<std::string, std::string>& operation_traits);

// Whether a replacement whose operation has replacement_traits keeps every
// safety trait of the operation it replaces, which has source_traits.
bool replacement_keeps_safety_traits(const std::string& source_traits,
                                     const std::string& replacement_traits);

// Whether a parameter of a replacement takes an argument the way the original
// parameter does: same type, writability and borrowing, and const wherever
// the original is const (a replacement may take less authority, never
// more).
bool same_parameter_contract(const ir::Parameter& replacement, const ir::Parameter& original);

// Whether a replacement declared to return `declared` may stand in for a
// call whose result type is `refined`: the same type, or a tensor of the
// same element type whose known rank and extents do not contradict it.
bool replacement_result_compatible(const Type& declared, const Type& refined);

} // namespace quidra::optimizer
