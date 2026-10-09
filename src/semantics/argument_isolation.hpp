#pragma once

// Argument isolation (D12): a by-value argument that names existing storage
// is passed borrowed, a read-only view of the caller's storage, instead of a
// copy. The view must not change while the call runs, since the callee holds
// a value. The effect analysis marks the arguments whose storage the call
// may write: through an & argument of the call, through the writes of
// the callee's summary carried through its & arguments and its receiver,
// through an argument or the receiver evaluated after it, or, for storage in
// a shared region, when the callee may change shared state. Such an argument
// is passed as a copy of its value at the call, released after the call
// (argument_isolation.cpp).
//
// Owns: the query the call lowering asks for each borrowed argument.

#include "quidra/checker.hpp"

namespace quidra::semantics {

class EffectSummaries;

class ArgumentIsolation {
public:
    // With `copy_every_argument`, every borrowed argument that names storage
    // is copied (an internal switch for differential tests).
    ArgumentIsolation(const CheckedProgram& checked, bool copy_every_argument);

    // Whether the borrowed argument `argument`, a call argument's expression
    // that names storage, must be passed as a copy.
    bool copies(const Expr& argument) const;

private:
    const EffectSummaries* summaries_;
    bool copy_every_argument_;
};

} // namespace quidra::semantics
