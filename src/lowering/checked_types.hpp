#pragma once

// type_of: an expression's checked type (CheckedProgram::expr_types), the
// type that lower converts its value to from its raw type (raw_types).

#include "quidra/checker.hpp"

#include <string>

namespace quidra::lowering {

// The IR name of a contextual constant's materialized value: the constant,
// the type and the slot's index.
inline std::string literal_constant_slot_name(const CheckedProgram& checked, std::size_t slot) {
    const auto& entry = checked.literal_constant_slot_list.at(slot);
    return entry.name + "." + type_name(entry.type) + "." + std::to_string(slot);
}

inline Type type_of(const CheckedProgram& checked, const Expr& e) {
    return checked.expr_types.at(&e);
}

// A user-defined len() has no builtin resolution and does not provide length facts.
inline const Expr* length_receiver(const CheckedProgram& checked, const Expr& expression) {
    const auto call = checked.call_resolutions.find(&expression);
    if (call == checked.call_resolutions.end() || call->second.builtin != BuiltinCallable::Len)
        return nullptr;
    return std::get<MethodCallExpr>(expression.data).receiver.get();
}

} // namespace quidra::lowering
