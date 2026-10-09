// Argument isolation (argument_isolation.hpp): the arguments come from the
// effect analysis, which decides them call by call.

#include "semantics/argument_isolation.hpp"
#include "semantics/effect_summary.hpp"

namespace quidra::semantics {

ArgumentIsolation::ArgumentIsolation(const CheckedProgram& checked, bool copy_every_argument)
    : summaries_(checked.effects.get()), copy_every_argument_(copy_every_argument) {}

bool ArgumentIsolation::copies(const Expr& argument) const {
    // Without summaries nothing is known about the call: copy.
    if (copy_every_argument_ || !summaries_) return true;
    return summaries_->isolated(argument);
}

} // namespace quidra::semantics
