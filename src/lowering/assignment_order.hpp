#pragma once

// AssignmentOrder: the plain assignments whose evaluation order can be
// observed, for the lowering of `target = value` (assignment_lowering.cpp).
//
// A plain assignment evaluates the target's subexpressions (the index and
// slice operands on the target's path, at every depth, left to right), then
// the right-hand side, then resolves the target from its root with those
// values and stores. Before, the right-hand side came first. The two orders
// differ only where the target's subexpressions and the right-hand side
// interact:
//   - tier 1 (every execution): one side may write a variable the other
//     side reads or writes (a `&` argument rooted in it, a receiver whose
//     receiver_effect writes, inside a method an implicit-receiver call
//     against a field read, or a call that receives a value holding a shared
//     region against a read of one), or both sides contain a call, whose
//     output order then differs;
//   - tier 2 (failure order only): one side may fail at run time (an index
//     read not proven in range, a slice, checked integer arithmetic, a real
//     power, a conversion, `try`, a call) and the other side contains a call
//     or may fail too.
// Every other assignment behaves the same in both orders, so the lowering
// keeps its emission order there (L9). Every answer errs toward listing a
// site. The census lists the same sites (tests/golden/census_keys.cpp, key
// assign-order). Defined in assignment_order.cpp.

#include "lowering/borrow_inference.hpp"
#include "quidra/checker.hpp"

namespace quidra::lowering {

class AssignmentOrder {
public:
    enum class Tier { none, every_execution, failure_order };

    AssignmentOrder(const CheckedProgram& checked, const BorrowInference& borrows)
        : checked_(checked), borrows_(borrows) {}

    // How the evaluation order of the plain assignment `assignment` can be
    // observed; none when it has no target subexpressions or when they and
    // the right-hand side do not interact.
    Tier classify(const AssignStmt& assignment) const;

    // The target's subexpressions of `target`, in evaluation order: the
    // index and slice operands of every index on its path, innermost base
    // first, each item's parts left to right.
    static std::vector<const Expr*> target_subexpressions(const Expr& target);

private:
    const CheckedProgram& checked_;
    const BorrowInference& borrows_;
};

} // namespace quidra::lowering
