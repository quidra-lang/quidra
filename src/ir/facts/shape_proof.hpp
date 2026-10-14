#pragma once

#include <cstddef>

namespace quidra { struct CheckedProgram; struct Expr; }
namespace quidra::ir::facts {

// Adapter to existing checker proofs. It borrows the checked program and
// expressions; it does not compute another set of shape facts.
class ShapeProof {
public:
    ShapeProof(const CheckedProgram& checked, bool force = false) : checked_(checked), force_(force) {}
    bool rank(const Expr* expression) const;
    bool axis(const Expr* expression, std::size_t axis) const;
private:
    const CheckedProgram& checked_;
    bool force_{};
};

} // namespace quidra::ir::facts
