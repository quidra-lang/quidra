#pragma once
#include "lowering/local_scope.hpp"
#include "lowering/ownership_expansion.hpp"

namespace quidra::lowering {

// An owned RHS or captured extent is not yet stored in its destination.
// Register it for ordinary return/try cleanup while its bounds execute.
template<class Check>
ir::ValueId validate_owned_range_candidate(
    ir::FunctionBuilder& builder, LocalScope& scope, ir::ValueId candidate,
    const Type& type, const Check& check) {
    if (!owns_managed(type)) { check(candidate); return candidate; }
    const auto slot = builder.hidden("range.pending");
    scope.local_type(slot) = type;
    builder.emit(ir::DeclareLocal{slot, type, {}, 0, 0});
    builder.emit(ir::StoreLocal{slot, candidate, type});
    check(candidate);
    const auto result = builder.fresh();
    builder.emit(ir::LoadLocal{result, slot, type, true});
    return result;
}

} // namespace quidra::lowering
