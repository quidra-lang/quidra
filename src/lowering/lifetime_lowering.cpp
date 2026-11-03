// Lifetime lowering: when a value is copied (Clone for values with
// value semantics, Retain for shared immutable storage), whether an
// expression's result is an owned temporary, and releasing one; and the
// release of the copies that value collection loops own and the unpinning of
// the arrays that reference loops pin.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId LifetimeLowering::copy_value(ValueId value,const Type& type) {
    return copy_owned(builder_,value,type);
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
    release_owned(builder_,value,source.type);
    builder_.emit(Jump{kept});
    builder_.enter(builder_.add_block(kept));
}

void LifetimeLowering::release_loop_sources() {
    for(auto it=loop_values_.rbegin();it!=loop_values_.rend();++it)
        release_owned(builder_,it->first,it->second);
    for(auto it=loop_references_.rbegin();it!=loop_references_.rend();++it)
        builder_.emit(BindReference{*it,0});
    for(auto it=loop_sources_.rbegin();it!=loop_sources_.rend();++it)
        release_loop_source(*it);
    for(auto it=loop_pins_.rbegin();it!=loop_pins_.rend();++it)
        builder_.emit(Unpin{*it});
}

bool LifetimeLowering::index_base_needs_snapshot(const IndexExpr& index) const {
    if (!owns_managed(type_of(checked_, *index.base)) ||
        expression_owns_result(*index.base))
        return false;
    const auto runs_code = [](const ExprPtr& operand) {
        return operand && expression_runs_code(*operand);
    };
    return std::any_of(index.items.begin(), index.items.end(), [&](const IndexPart& item) {
        return runs_code(item.index) || runs_code(item.start) ||
               runs_code(item.stop) || runs_code(item.step);
    });
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
    if (!owns_managed(lifetime_type)) return false;
    if(const auto* projection=std::get_if<TupleProjectionExpr>(&expression.data))return tuple_moves(projection->subject);
    if (std::holds_alternative<StringExpr>(expression.data)) {
        return false;
    }
    if (const auto* name=std::get_if<NameExpr>(&expression.data)) {
        return scope_.bit_place(name->name)!=nullptr;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        // Tensor .grad materializes a fresh tensor clone. Treat it as an owned
        // temporary so chained indexing/item() and discarded gradient reads
        // release that clone.
        if (checked_.tensor_grad_accesses.contains(&expression)) return true;
        // An error's member is a new value (a retained string, a union).
        if (checked_.error_members.contains(&expression)) return true;
        return expression_owns_result(*member->base);
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        const auto base_kind = type_of(checked_, *index->base).kind;
        if (base_kind == TypeKind::Tensor || base_kind == TypeKind::String ||
            base_kind == TypeKind::Bin) return true;
        // An array slice is a new array.
        if (index->items.size() == 1 && index->items.front().slice) return true;
        // Index arrays and boolean masks also materialize independent
        // arrays, even when the source is a borrowed local or field.
        if (base_kind == TypeKind::Array && index->items.size() == 1 &&
            index->items.front().index &&
            type_of(checked_, *index->items.front().index).kind == TypeKind::Array)
            return true;
        // Scalar reads from an owned snapshot are copied out before that
        // snapshot is released, and therefore own the copied value too.
        if (index_base_needs_snapshot(*index)) return true;
        return expression_owns_result(*index->base);
    }
    if (const auto* tried = std::get_if<TryExpr>(&expression.data)) {
        return expression_owns_result(*tried->value);
    }
    if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
        const auto receiver_kind = type_of(checked_, *method->receiver).kind;
        if (receiver_kind == TypeKind::String && method->method == "string") {
            return expression_owns_result(*method->receiver);
        }
        return true;
    }
    if (std::holds_alternative<CallExpr>(expression.data)) {
        // A call owns its result; error(...) makes a new error.
        return true;
    }
    return true;
}

void LifetimeLowering::release_temporary(const Expr& expression, ValueId value) {
    if (value != 0 && expression_owns_result(expression)) {
        release_owned(builder_,value,type_of(checked_,expression));
    }
}

} // namespace quidra::lowering
