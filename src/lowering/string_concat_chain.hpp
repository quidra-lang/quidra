#pragma once

// string_concat_chain: the operands of a chain of string `+`, left to right
// (a + b + c gives a, b, c): an operand whose checked type is string and that
// is itself a `+` is flattened, any other expression is an operand. The
// chain is walked with an explicit stack, not the host's, so a long chain
// costs no stack depth. A binary `+` of strings and the assignment
// s = s + ... both flatten their right-hand side this way
// (string_concat_chain.cpp).

#include "quidra/checker.hpp"
#include <vector>

namespace quidra::lowering {

std::vector<const Expr*> string_concat_chain(const Expr& root, const CheckedProgram& checked);

} // namespace quidra::lowering
