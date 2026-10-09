// Coverage evidence for the golden corpus (the `census` view; README.md, Inputs).
//
// One line per row so that tests/golden/corpus.py can aggregate the tables
// across the corpus and tests/golden/expect.py can select entries by them:
//   alt   <index>:<name> <lowered count> <optimized count>   (all 185, zeros included)
//   sig   <name> <key> <lowered count> <optimized count>    (type-signature keys)
//   event <kind> <key> <count>                              (optimizer effects, derived
//                                                            from lowered vs optimized)
//   count <name> <lowered count> <optimized count>          (runtime checks by kind and
//                                                            loop operations; census_count_names)
//   op    <kind> <operation> <lowered count> <optimized count>  (numeric operations per kind)
//   kind  <type> <expressions>                              (numeric kinds after checking)
//   key   <name> <sites>                                    (the listed sites of each key;
//                                                            census_key_names)
//   site  <name> <file>:<line>:<column>                     (one per listed site, sorted)
#pragma once

#include "quidra/checker.hpp"
#include "quidra/ir.hpp"

#include <string>
#include <vector>

namespace quidra::golden {

// Either module may be null when its stage failed; its counts are then absent.
// The checked program gives the source-level keys and kinds; without one
// (a hand-built module, a failed check) they are zero.
std::string census_view(const ir::Module* lowered, const ir::Module* optimized,
                        const CheckedProgram* checked = nullptr);

// The same over several modules (the submissions of a REPL session).
std::string census_view(const std::vector<const ir::Module*>& lowered,
                        const std::vector<const ir::Module*>& optimized,
                        const std::vector<const CheckedProgram*>& checked = {});

} // namespace quidra::golden
