#pragma once

// Value uses read from instruction text: an instruction uses value N when
// its instruction_text contains %N not followed by another digit. These
// scans stand in for def-use information, which the IR does not have yet;
// they see only the operands that instruction_text renders.

#include "quidra/ir/module.hpp"

#include <optional>

namespace quidra::optimizer {

bool instruction_mentions_value(const ir::Instruction& instruction, ir::ValueId value);

// Whether `value` is used after `current` in its block, or anywhere in
// another block of `function`; a release of it is not a use. Also true when
// `current` is not an instruction of the function.
bool has_later_or_cross_block_use(const ir::Function& function, const ir::Instruction* current,
                                  ir::ValueId value);

// The value after the largest %N in the text of the function: the first
// value a rewrite may define. None when there is no such ValueId.
std::optional<ir::ValueId> next_rewrite_value(const ir::Function& function);

} // namespace quidra::optimizer
