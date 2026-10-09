// Lifetime lowering: when a value is copied (Clone for values with
// value semantics, Retain for shared immutable storage), whether an
// expression's result is an owned temporary, and releasing one; and the
// release of the copies that value collection loops own and the unpinning of
// the arrays that reference loops pin.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId LifetimeLowering::copy_value(ValueId v,const Type& t) {
    if (requires_value_clone(t)) {
        auto out=builder_.fresh();
        builder_.emit(Clone{out,v,t});
        return out;
    }
    if (uses_shared_immutable_storage(t)) {
        auto out=builder_.fresh();
        builder_.emit(Retain{out,v,t});
        return out;
    }
    return v;
}

void LifetimeLowering::release_loop_source(const LoopSource& source) {
    auto owned=builder_.fresh();
    builder_.emit(LoadLocal{owned,source.owned,Type::simple(TypeKind::Bool)});
    const auto release=builder_.label("for.source.release");
    const auto kept=builder_.label("for.source.kept");
    builder_.emit(Branch{owned,release,kept});
    builder_.enter(builder_.add_block(release));
    auto value=builder_.fresh();
    builder_.emit(LoadLocal{value,source.source,source.type});
    builder_.emit(Release{value,source.type});
    builder_.emit(Jump{kept});
    builder_.enter(builder_.add_block(kept));
}

void LifetimeLowering::release_loop_sources() {
    for(auto it=loop_sources_.rbegin();it!=loop_sources_.rend();++it)
        release_loop_source(*it);
    for(auto it=loop_pins_.rbegin();it!=loop_pins_.rend();++it)
        builder_.emit(Unpin{*it});
}

bool LifetimeLowering::expression_owns_result(const Expr& expression) const {
    if (checked_.enum_constructions.contains(&expression)) return true;
    const auto type = type_of(checked_, expression);
    // A contextual fail-fast expression is source-visible as T | error but
    // type_of(expression) is the narrowed success type T.  Ownership still
    // belongs to the raw temporary container.  Use the raw type only for
    // the lifetime gate; the structural cases below continue to distinguish
    // owned temporaries from borrowed names/members.
    const auto raw = checked_.raw_types.find(&expression);
    const auto& lifetime_type =
        checked_.fail_fast_expressions.contains(&expression) &&
                raw != checked_.raw_types.end()
            ? raw->second
            : type;
    if (!requires_lifetime_management(lifetime_type)) return false;
    if (std::holds_alternative<StringExpr>(expression.data)) {
        return false;
    }
    if (std::holds_alternative<NameExpr>(expression.data)) {
        return false;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        // Tensor .grad materializes a fresh tensor clone. Treat it as an owned
        // temporary so chained indexing/item() and discarded gradient reads
        // release that clone.
        if (checked_.tensor_grad_accesses.contains(&expression)) return true;
        return expression_owns_result(*member->base);
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        const auto base_kind = type_of(checked_, *index->base).kind;
        if (base_kind == TypeKind::Tensor || base_kind == TypeKind::String ||
            base_kind == TypeKind::Bin) return true;
        return expression_owns_result(*index->base);
    }
    if (const auto* tried = std::get_if<TryExpr>(&expression.data)) {
        return expression_owns_result(*tried->value);
    }
    if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
        const auto receiver_kind = type_of(checked_, *method->receiver).kind;
        if ((receiver_kind == TypeKind::String || receiver_kind == TypeKind::Error) &&
            method->method == "string") {
            return expression_owns_result(*method->receiver);
        }
        return true;
    }
    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        const auto it = checked_.call_resolutions.find(&expression);
        if (it != checked_.call_resolutions.end() &&
            it->second.kind == CallKind::Constructor &&
            it->second.type.kind == TypeKind::Error &&
            !call->args.empty()) {
            return expression_owns_result(*call->args[0].value);
        }
        return true;
    }
    return true;
}

void LifetimeLowering::release_temporary(const Expr& expression, ValueId value) {
    if (value != 0 && expression_owns_result(expression)) {
        builder_.emit(Release{value,type_of(checked_,expression)});
    }
}

} // namespace quidra::lowering
