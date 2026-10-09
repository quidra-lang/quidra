#pragma once

// NestingDepth: the depth of the lowering's two recursions, expressions and
// statements, each held under its nesting budget (nesting_budget.hpp).
// Lowering overflows the stack on input the checker accepts: an `and` chain
// of 900 links (5,425 bytes) checks clean and segfaults without the budget.
// A guard counts one level while it lives and reports NESTING_DEPTH beyond
// the budget.

#include "nesting_budget.hpp"
#include "quidra/diagnostic.hpp"
#include <cstddef>

namespace quidra::lowering {

class NestingDepth {
public:
    nesting::DepthGuard expression(const SourceSpan& span) {
        return nesting::DepthGuard(expression_, nesting::max_expression_depth, span, "Expression");
    }
    nesting::DepthGuard statement(const SourceSpan& span) {
        return nesting::DepthGuard(statement_, nesting::max_statement_depth, span, "Statement");
    }

private:
    std::size_t expression_{};
    std::size_t statement_{};
};

} // namespace quidra::lowering
