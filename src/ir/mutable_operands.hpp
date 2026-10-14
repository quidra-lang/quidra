#pragma once
#include "quidra/ir/visit.hpp"
namespace quidra::ir {
// Renaming adapter over authored typed operands. Ownership roles are supplied
// by the const visitor; this interface never changes their classification.
class MutableOperandVisitor {
public:
    virtual ~MutableOperandVisitor() = default;
    virtual void definition(ValueId&) {}
    virtual void use(ValueId&) {}
    virtual void metadata_value(ValueId&) {}
    virtual void local(std::string&, LocalRole) {}
    virtual void successor(std::string&) {}
};
void mutate_operands(Instruction&, MutableOperandVisitor&);
} // namespace quidra::ir
