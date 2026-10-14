#pragma once

// Typed semantic operands of an instruction. The traversal emits definitions
// first, then uses in authored field order, then storage/control names. Nested
// records and optional operands are traversed explicitly. Types, opcodes,
// locations, field indices and diagnostic names are not operands.
//
// The visitor owns nothing. Names are borrowed from the instruction and remain
// valid only while that instruction is unchanged. A visitor may ignore roles
// it does not need. Reading an address is an SSA use; taking a local's address
// is distinct from reading its stored value. This is not an effect summary.
// ReferenceRead/ReferenceWrite access the target of a reference binding;
// Read/Write access the binding itself (including rebinding its address).

#include "quidra/ir/instruction.hpp"

#include <string_view>
#include <cstdint>

namespace quidra::ir {

enum class LocalRole { Declaration, Read, Write, Address, ReadWrite, ReferenceRead, ReferenceWrite };

struct Module;
enum class OperandRole : std::uint8_t { Read, Borrow, Consume, Address, Condition, AutogradTarget };
// Read describes an SSA observation, not an ownership theorem. Borrow/Consume
// describe a known whole-value ownership contract. Partial/conditional transfer
// and unavailable typed contracts retain the use with role_available=false.
struct OperandUse {
    OperandRole role{OperandRole::Read};
    bool role_available{true};
    bool operator==(const OperandUse&) const = default;
};
struct OperandContext { const Module* module{}; };
namespace detail { struct ScopedOperandContext; }

class OperandVisitor {
public:
    virtual ~OperandVisitor() = default;
    virtual void definition(ValueId) {}
    virtual void use(ValueId) {}
    // Additive dispatch preserves visitors overriding only the legacy overload.
    virtual void use(ValueId value, OperandUse) { use(value); }
    virtual void local(std::string_view, LocalRole) {}
    virtual void successor(std::string_view) {}
    const OperandContext* context() const noexcept { return context_; }
private:
    friend struct detail::ScopedOperandContext;
    const OperandContext* context_{};
};

// Dispatches through InstructionTraits<T>::visit_operands. There is no generic
// empty fallback: src/ir/visit.cpp proves that every Instruction alternative
// has an explicitly authored map, including operand-free instructions.
void visit_operands(const Instruction& instruction, OperandVisitor& visitor);
// Context and Module are non-owning and must live through this call. The
// visitor's prior context is restored on nested traversal and exceptions.
void visit_operands(const Instruction& instruction, OperandVisitor& visitor,
                    const OperandContext& context);

} // namespace quidra::ir
