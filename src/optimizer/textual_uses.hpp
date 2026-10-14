#pragma once

// Conservative value-use helpers over the exhaustive typed operand visitor.
// Definitions and literal/diagnostic spellings are not uses. The historical
// file and helper names remain while CFG def-use and liveness are introduced.

#include "quidra/ir/module.hpp"

#include <optional>

namespace quidra::optimizer {

bool instruction_mentions_value(const ir::Instruction& instruction, ir::ValueId value);

// Whether `value` is used after `current` in its block, or anywhere in
// another block of `function`; a release of it is not a use. Also true when
// `current` is not an instruction of the function.
bool has_later_or_cross_block_use(const ir::Function& function, const ir::Instruction* current,
                                  ir::ValueId value);

// The Function's persistent allocation cursor, not a scan of current IDs.
// Imported IR is seeded once at the optimizer boundary. None on exhaustion.
std::optional<ir::ValueId> next_rewrite_value(const ir::Function& function);

} // namespace quidra::optimizer
