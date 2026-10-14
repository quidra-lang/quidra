#pragma once

// instruction_text: one instruction on one line, as `quidra ir` prints it
// (ir::dump). Every instruction kind has an explicit rendering.
//
// The text is a diagnostic view, not an operand inventory. Optimizer analyses
// read semantic operands through ir::visit_operands instead of parsing it.

#include "quidra/ir/instruction.hpp"

#include <string>

namespace quidra::ir {

std::string instruction_text(const Instruction& instruction);

} // namespace quidra::ir
