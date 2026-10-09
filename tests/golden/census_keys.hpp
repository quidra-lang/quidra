// The listed sites of the census (census.hpp, the `key` and `site` rows) and
// the names of its always-printed counts.
//
// A key lists the places a language change may alter, so that the change's
// expected differences (tests/golden/expect.py, census predicates such as
// `key.assign-order > 0`) and its review lists come from the parent's census.
// Every list is a sound over-approximation of the sites where the change can
// be observed; README.md (Inputs) defines each key.
#pragma once

#include "quidra/checker.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::golden {

// Key name -> its sites, each FILE:LINE:COLUMN.
using CensusKeys = std::map<std::string, std::vector<std::string>>;

// The `count` rows printed for every entry, zeros included.
inline constexpr std::array<std::string_view, 16> census_count_names{
    "check.assert",           "check.bounds.address", "check.bounds.array",
    "check.bounds.bin",       "check.bounds.string",  "check.conversion",
    "check.divide",           "check.extent",         "check.initialized.array",
    "check.overflow",         "check.range-step",     "check.shape",
    "check.slice",            "loop.bigint-arithmetic", "loop.overflow-checked",
    "loop.overflow-checked-step"};

// The `key` rows printed for every entry, zeros included.
inline constexpr std::array<std::string_view, 12> census_key_names{
    "argument-isolation-copy",
    "assign-order",
    "assign-order.tier1",
    "assign-order.tier2",
    "autograd-target-copy",
    "backward-unvisitable-target",
    "collection-loop-writes-iterable",
    "compound-store-across-call",
    "reference-loop-alias-call",
    "reference-loop-alias-call.statements",
    "reference-loop-current-element-alias",
    "typed-constant-retyped"};

// The source-level keys of a checked program, added to `keys`, and the
// numeric kinds of its checked expressions (type name -> expressions; tensor
// element kinds as `tensor.<type>`), added to `kinds`.
void source_keys(const CheckedProgram& checked, CensusKeys& keys,
                 std::map<std::string, std::size_t>& kinds);

} // namespace quidra::golden
