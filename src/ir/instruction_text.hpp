#pragma once

// instruction_text: one instruction on one line, as `quidra ir` prints it
// (ir::dump) and as the optimizer reads value uses (%N) out of it.
//
// The text is part of `quidra ir`'s output and of what the optimizer decides,
// so it changes only together with both. An instruction kind without a
// rendering gives the empty string.

#include "quidra/ir/instruction.hpp"

#include <string>

namespace quidra::ir {

std::string instruction_text(const Instruction& instruction);

} // namespace quidra::ir
