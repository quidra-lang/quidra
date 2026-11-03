#include "ir/facts/shape_proof.hpp"
#include "quidra/checker.hpp"

namespace quidra::ir::facts {
bool ShapeProof::rank(const Expr* expression) const {
    if (force_ || !expression) return false;
    const auto found = checked_.proven_shape_constraints.find(expression);
    return found != checked_.proven_shape_constraints.end() && found->second.rank;
}
bool ShapeProof::axis(const Expr* expression, std::size_t axis) const {
    if (force_ || !expression) return false;
    const auto found = checked_.proven_shape_constraints.find(expression);
    return found != checked_.proven_shape_constraints.end() &&
        axis < found->second.axes.size() && found->second.axes[axis];
}
} // namespace quidra::ir::facts
