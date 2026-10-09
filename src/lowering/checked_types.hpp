#pragma once

// type_of: an expression's checked type (CheckedProgram::expr_types), the
// type that lower converts its value to from its raw type (raw_types).

#include "quidra/checker.hpp"

namespace quidra::lowering {

inline Type type_of(const CheckedProgram& checked, const Expr& e) {
    return checked.expr_types.at(&e);
}

} // namespace quidra::lowering
