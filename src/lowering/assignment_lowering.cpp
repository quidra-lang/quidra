// Assignment lowering: compound assignment (x op= v), the in-place appends
// that reuse uniquely owned storage (s = s + ..., xs = xs.append(v)) with
// their copying fallbacks, and plain stores to a local, a reference, a field
// or an element. The fast and fallback paths each lower the right-hand side,
// so it is lowered twice, once per path.
//
// A plain assignment evaluates its target's subexpressions (the index
// operands on the target's path), then the right-hand side, then resolves
// the target from its root with those values and stores. Where the two
// cannot be told apart (assignment_order.hpp), the subexpressions are
// lowered with the target after the right-hand side, as before.
//
// A compound assignment to an element or a field evaluates the target's
// subexpressions once, reads the element, evaluates the right-hand side and
// applies the operation. When the right-hand side may write the target's
// root storage (storage_writes.hpp), it may replace or resize the array or
// object the first resolution addressed, so the store resolves the target
// again from its root with the index values computed once, and a managed
// old value is retained across the right-hand side.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "lowering/string_concat_chain.hpp"
#include "quidra/language.hpp"
#include <vector>

namespace quidra::lowering {

void AssignmentLowering::lower_assignment(const Stmt& s, const AssignStmt& n) {
    const auto t=type_of(checked_,*n.target);
    if(!n.compound_op.empty()) return lower_compound_assignment(s,n,t);
    if(lower_string_append_assignment(n,t)) return;
    if(lower_array_append_assignment(s,n,t)) return;
    lower_plain_assignment(s,n,t);
}

void AssignmentLowering::lower_compound_assignment(
    const Stmt& s, const AssignStmt& n, const Type& t) {
    // Compiler-owned Map/Set fields are all initialized by their
    // generated class construction. Scalar compound updates inside
    // those methods therefore do not need to take a tracked field
    // address merely to preserve definite-initialization checks.
    // Keep the ordinary checked Binary lowering, so overflow and
    // every other public arithmetic semantic remain unchanged.
    const bool standard_collection_class =
        enclosing_class_.is_standard_collection();
    if(standard_collection_class && t.kind==TypeKind::Int64){
        if(const auto* field_name=std::get_if<NameExpr>(&n.target->data);
           field_name && checked_.field_accesses.contains(n.target.get())){
            const auto& field=checked_.field_accesses.at(n.target.get());
            auto object=load_receiver(builder_,enclosing_class_);
            auto old=builder_.fresh();
            builder_.emit(
                FieldGet{old,object,field.index,t});
            auto rhs=lowerer_.lower(*n.value);
            auto result=builder_.fresh();
            builder_.emit(Binary{
                result,n.compound_op,old,rhs,t,t,
                static_cast<std::uint32_t>(s.span.start.line),
                static_cast<std::uint32_t>(s.span.start.column)});
            lifetime_.release_temporary(*n.value,rhs);
            builder_.emit(
                FieldSet{object,field.index,result,t});
            return;
        }
    }
    if(const auto* name=std::get_if<NameExpr>(&n.target->data);
       name && !checked_.field_accesses.contains(n.target.get()) &&
       !scope_.is_source_reference(name->name)){
        facts_.array_bounds.invalidate_length_relation(name->name);
        lowerer_.initialization_flags().check(*n.target);
        auto old=builder_.fresh();
        const auto local_name=scope_.source_local(name->name);
        builder_.emit(LoadLocal{old,local_name,t});
        if(t.kind==TypeKind::String && n.compound_op=="+" &&
           !expression_contains_writable_argument(*n.value)){
            auto can_move=builder_.fresh();
            builder_.emit(StringCanAppendMove{can_move,old});
            const auto fast=builder_.label("string.plus_assign.move");
            const auto fallback=builder_.label("string.plus_assign.copy");
            const auto done=builder_.label("string.plus_assign.done");
            builder_.emit(Branch{can_move,fast,fallback});

            builder_.enter(builder_.add_block(fast));
            auto rhs=lowerer_.lower(*n.value);
            auto moved=builder_.fresh();
            builder_.emit(
                StringAppendMove{moved,old,std::vector<ValueId>{rhs}});
            lifetime_.release_temporary(*n.value,rhs);
            builder_.emit(
                StoreLocal{local_name,moved,t,false,true});
            builder_.emit(Jump{done});

            builder_.enter(builder_.add_block(fallback));
            auto copied_rhs=lowerer_.lower(*n.value);
            auto result=builder_.fresh();
            builder_.emit(Binary{
                result,n.compound_op,old,copied_rhs,t,t,
                static_cast<std::uint32_t>(s.span.start.line),
                static_cast<std::uint32_t>(s.span.start.column)});
            lifetime_.release_temporary(*n.value,copied_rhs);
            builder_.emit(StoreLocal{local_name,result,t});
            builder_.emit(Jump{done});

            builder_.enter(builder_.add_block(done));
            return;
        }
        auto rhs=lowerer_.lower(*n.value);
        auto result=builder_.fresh();
        builder_.emit(Binary{
            result,n.compound_op,old,rhs,t,t,
            static_cast<std::uint32_t>(s.span.start.line),
            static_cast<std::uint32_t>(s.span.start.column)});
        lifetime_.release_temporary(*n.value,rhs);
        builder_.emit(
            StoreLocal{local_name,result,t});
        return;
    }
    if((lowerer_.options().reresolve_every_compound_store ||
        storage_writes_.value_may_write_target_root(*n.target,*n.value)) &&
       target_path_replayable(*n.target))
        return lower_compound_store_into_current_storage(s,n,t);
    auto address=lowerer_.lower_address(*n.target);
    auto old=builder_.fresh(); builder_.emit(LoadAddress{old,address,t});
    auto rhs=lowerer_.lower(*n.value);
    auto result=builder_.fresh(); builder_.emit(Binary{result,n.compound_op,old,rhs,t,t,static_cast<std::uint32_t>(s.span.start.line),static_cast<std::uint32_t>(s.span.start.column)});
    lifetime_.release_temporary(*n.value,rhs);
    builder_.emit(StoreAddress{address,result,t});
}

void AssignmentLowering::lower_compound_store_into_current_storage(
    const Stmt& s, const AssignStmt& n, const Type& t) {
    std::vector<ValueId> indices;
    std::optional<std::size_t> first;
    auto address=lower_target_address(*n.target,indices,first);
    auto old=builder_.fresh();
    builder_.emit(LoadAddress{old,address,t});
    const bool managed=requires_lifetime_management(t);
    if(managed){
        auto kept=builder_.fresh();
        builder_.emit(Retain{kept,old,t});
        old=kept;
    }
    auto rhs=lowerer_.lower(*n.value);
    auto result=builder_.fresh();
    builder_.emit(Binary{
        result,n.compound_op,old,rhs,t,t,
        static_cast<std::uint32_t>(s.span.start.line),
        static_cast<std::uint32_t>(s.span.start.column)});
    lifetime_.release_temporary(*n.value,rhs);
    if(managed) builder_.emit(Release{old,t});
    std::optional<std::size_t> replay=0;
    auto current=lower_target_address(*n.target,indices,replay);
    builder_.emit(StoreAddress{current,result,t});
}

bool AssignmentLowering::target_path_replayable(const Expr& expression) const {
    // A base on the path: a binding, or a field or array element of one,
    // which a second resolution reads without side effects or conversions.
    const auto base_replayable=[&](const auto& self,const Expr& e)->bool{
        const auto raw=checked_.raw_types.find(&e);
        if(raw==checked_.raw_types.end() || raw->second!=type_of(checked_,e))
            return false;
        if(const auto* name=std::get_if<NameExpr>(&e.data))
            return !is_builtin_text_constant(name->name) &&
                   !checked_.function_references.contains(&e);
        if(const auto* member=std::get_if<MemberExpr>(&e.data))
            return !checked_.tensor_grad_accesses.contains(&e) &&
                   checked_.field_accesses.contains(&e) &&
                   !lifetime_.expression_owns_result(*member->base) &&
                   self(self,*member->base);
        if(const auto* index=std::get_if<IndexExpr>(&e.data))
            return type_of(checked_,*index->base).kind==TypeKind::Array &&
                   index->items.size()==1 && !index->items.front().slice &&
                   index->items.front().index &&
                   !lifetime_.expression_owns_result(*index->base) &&
                   self(self,*index->base);
        return false;
    };
    if(const auto* member=std::get_if<MemberExpr>(&expression.data))
        return !checked_.tensor_grad_accesses.contains(&expression) &&
               checked_.field_accesses.contains(&expression) &&
               base_replayable(base_replayable,*member->base);
    if(const auto* index=std::get_if<IndexExpr>(&expression.data)){
        const auto kind=type_of(checked_,*index->base).kind;
        return (kind==TypeKind::Array || kind==TypeKind::Bin) &&
               index->items.size()==1 && !index->items.front().slice &&
               index->items.front().index &&
               base_replayable(base_replayable,*index->base);
    }
    return false;
}

ValueId AssignmentLowering::lower_target_address(
    const Expr& target, std::vector<ValueId>& indices,
    std::optional<std::size_t>& replay) {
    if(const auto* member=std::get_if<MemberExpr>(&target.data)){
        auto object=lower_path_value(*member->base,indices,replay),out=builder_.fresh();
        if(!replay) lowerer_.initialization_flags().check_field(target,object);
        builder_.emit(AddressField{out,object,checked_.field_accesses.at(&target).index});
        return out;
    }
    const auto& ix=std::get<IndexExpr>(target.data);
    const auto array_type=type_of(checked_,*ix.base);
    auto array=lower_path_value(*ix.base,indices,replay);
    ValueId index;
    if(replay) index=indices.at((*replay)++);
    else { index=lowerer_.lower_int64(*ix.items.front().index); indices.push_back(index); }
    auto out=builder_.fresh();
    const auto element_type=array_type.kind==TypeKind::Bin
        ? Type::simple(TypeKind::Nat8)
        : *array_type.first;
    // An element index failure reports the index operand.
    const auto& at=ix.items.front().index->span.start;
    builder_.emit(AddressElement{
        out,array,index,array_type,element_type,array_type.kind==TypeKind::Bin,
        static_cast<std::uint32_t>(at.line),
        static_cast<std::uint32_t>(at.column)});
    return out;
}

ValueId AssignmentLowering::lower_path_value(
    const Expr& expression, std::vector<ValueId>& indices,
    std::optional<std::size_t>& replay) {
    if(std::holds_alternative<NameExpr>(expression.data))
        return lowerer_.lower(expression);
    if(const auto* member=std::get_if<MemberExpr>(&expression.data)){
        auto object=lower_path_value(*member->base,indices,replay),out=builder_.fresh();
        const auto& info=checked_.field_accesses.at(&expression);
        builder_.emit(FieldGet{out,object,info.index,info.type});
        return out;
    }
    // An array element, read as lower_index reads it. A replay reads it from
    // whatever array the base holds now, so it proves neither bounds nor
    // initialization.
    const auto& ix=std::get<IndexExpr>(expression.data);
    const auto& item=*ix.items.front().index;
    const bool standard_collection_array =
        enclosing_class_.is_standard_collection() &&
        checked_.field_accesses.contains(ix.base.get());
    const bool initialization_proven = !replay &&
        (facts_.array_initialization.fully_initialized(*ix.base) ||
         standard_collection_array);
    auto array=lower_path_value(*ix.base,indices,replay),out=builder_.fresh();
    ValueId index;
    if(replay) index=indices.at((*replay)++);
    else { index=lowerer_.lower_int64(item); indices.push_back(index); }
    const auto initialization_guard = initialization_proven || replay
        ? std::optional<ValueId>{}
        : facts_.array_initialization.reference_guard(*ix.base);
    const bool bounds_proven = !replay &&
        (checked_.bounds_proven.contains(&item) || standard_collection_array ||
         facts_.array_bounds.proven_by_range_loop(*ix.base, item));
    const auto bounds_guard = bounds_proven || replay
        ? std::optional<ValueId>{}
        : facts_.array_bounds.reference_guard(*ix.base, item);
    builder_.emit(ArrayGet{
        out,array,index,checked_.raw_types.at(&expression),
        static_cast<std::uint32_t>(item.span.start.line),
        static_cast<std::uint32_t>(item.span.start.column),
        initialization_proven, bounds_proven,
        initialization_guard, bounds_guard});
    return out;
}

bool AssignmentLowering::lower_string_append_assignment(const AssignStmt& n, const Type& t) {
    // s = s + ... may reuse uniquely-owned string storage. Shared strings
    // keep ordinary immutable value semantics through the fallback path.
    if(t.kind==TypeKind::String && !lowerer_.initialization_flags().active() &&
       !expression_contains_writable_argument(*n.value)){
        const auto* target_name=std::get_if<NameExpr>(&n.target->data);
        if(target_name && !checked_.field_accesses.contains(n.target.get()) &&
           !scope_.is_source_reference(target_name->name)){
            const auto parts=string_concat_chain(*n.value,checked_);
            const auto* first_name=parts.empty()
                ? nullptr
                : std::get_if<NameExpr>(&parts.front()->data);
            if(first_name && source_key(*first_name)==source_key(*target_name) &&
               parts.size()>1){
                const auto local_name=scope_.source_local(target_name->name);
                auto source=builder_.fresh();
                builder_.emit(LoadLocal{source,local_name,t});
                auto can_move=builder_.fresh();
                builder_.emit(StringCanAppendMove{can_move,source});

                const auto fast=builder_.label("string.append.move");
                const auto fallback=builder_.label("string.append.copy");
                const auto done=builder_.label("string.append.done");
                builder_.emit(Branch{can_move,fast,fallback});

                builder_.enter(builder_.add_block(fast));
                std::vector<ValueId> suffixes;
                suffixes.reserve(parts.size()-1);
                for(std::size_t i=1;i<parts.size();++i)
                    suffixes.push_back(lowerer_.lower(*parts[i]));
                auto moved=builder_.fresh();
                builder_.emit(
                    StringAppendMove{moved,source,suffixes});
                for(std::size_t i=1;i<parts.size();++i)
                    lifetime_.release_temporary(*parts[i],suffixes[i-1]);
                builder_.emit(
                    StoreLocal{local_name,moved,t,false,true});
                builder_.emit(Jump{done});

                builder_.enter(builder_.add_block(fallback));
                auto copied=lowerer_.lower_into(*n.value,t);
                builder_.emit(StoreLocal{local_name,copied,t});
                builder_.emit(Jump{done});

                builder_.enter(builder_.add_block(done));
                return true;
            }
        }
    }
    return false;
}

bool AssignmentLowering::lower_array_append_assignment(
    const Stmt& s, const AssignStmt& n, const Type& t) {
    // xs = xs.append(value) can reuse xs only when the runtime proves that
    // the allocation is uniquely owned, fully initialized, and not pinned
    // by an interior reference. Otherwise the ordinary value-copy path is
    // preserved exactly.
    if(t.kind==TypeKind::Array && t.length==-1 && !lowerer_.initialization_flags().active()){
        const auto* target_name=std::get_if<NameExpr>(&n.target->data);
        const auto* append=std::get_if<MethodCallExpr>(&n.value->data);
        const auto* receiver_name=append
            ? std::get_if<NameExpr>(&append->receiver->data)
            : nullptr;
        const auto target_field =
            target_name ? checked_.field_accesses.find(n.target.get())
                        : checked_.field_accesses.end();
        const auto receiver_field =
            (append && receiver_name)
                ? checked_.field_accesses.find(append->receiver.get())
                : checked_.field_accesses.end();
        const bool simple_append_value = append && append->args.size()==1 && (
            std::holds_alternative<IntegerExpr>(append->args[0].value->data) ||
            std::holds_alternative<RealLiteralExpr>(append->args[0].value->data) ||
            std::holds_alternative<StringExpr>(append->args[0].value->data) ||
            std::holds_alternative<BoolExpr>(append->args[0].value->data) ||
            std::holds_alternative<NoneExpr>(append->args[0].value->data) ||
            std::holds_alternative<NameExpr>(append->args[0].value->data));

        // The compiler-generated map/set storage uses class fields. Reuse
        // unique backing storage for field = field.append(simple_value)
        // without changing source evaluation order. The runtime still
        // rejects the move when aliases or interior references exist.
        if(target_name && append && receiver_name && simple_append_value &&
           append->method=="append" && append->args.size()==1 &&
           receiver_name->name==target_name->name &&
           target_field!=checked_.field_accesses.end() &&
           receiver_field!=checked_.field_accesses.end() &&
           target_field->second.owner==receiver_field->second.owner &&
           target_field->second.index==receiver_field->second.index){
            auto object=load_receiver(builder_,enclosing_class_);
            auto source=builder_.fresh();
            builder_.emit(FieldGet{
                source,object,target_field->second.index,t});
            auto can_move=builder_.fresh();
            builder_.emit(ArrayCanAppendMove{can_move,source});

            const auto fast=builder_.label("append.field.move");
            const auto fallback=builder_.label("append.field.copy");
            const auto done=builder_.label("append.field.done");
            builder_.emit(Branch{can_move,fast,fallback});

            builder_.enter(builder_.add_block(fast));
            auto old_length=builder_.fresh();
            builder_.emit(ArrayLength{old_length,source});
            auto appended=lowerer_.lower_into(*append->args[0].value,*t.first);
            auto grown=builder_.fresh();
            builder_.emit(ArrayGrowMove{grown,source,t});
            builder_.emit(ArraySet{
                grown,old_length,appended,*t.first,
                static_cast<std::uint32_t>(s.span.start.line),
                static_cast<std::uint32_t>(s.span.start.column),true});
            builder_.emit(FieldSet{
                object,target_field->second.index,grown,t,true});
            builder_.emit(Jump{done});

            builder_.enter(builder_.add_block(fallback));
            auto copied=lowerer_.lower_into(*n.value,t);
            builder_.emit(FieldSet{
                object,target_field->second.index,copied,t});
            builder_.emit(Jump{done});

            builder_.enter(builder_.add_block(done));
            return true;
        }

        if(target_name && append && receiver_name &&
           append->method=="append" && append->args.size()==1 &&
           source_key(*receiver_name)==source_key(*target_name) &&
           !checked_.field_accesses.contains(n.target.get()) &&
           !scope_.is_source_reference(target_name->name)){
            const auto local_name=scope_.source_local(target_name->name);
            auto source=builder_.fresh();
            builder_.emit(LoadLocal{source,local_name,t});
            auto can_move=builder_.fresh();
            builder_.emit(ArrayCanAppendMove{can_move,source});

            const auto fast=builder_.label("append.move");
            const auto fallback=builder_.label("append.copy");
            const auto done=builder_.label("append.done");
            builder_.emit(Branch{can_move,fast,fallback});

            builder_.enter(builder_.add_block(fast));
            auto old_length=builder_.fresh();
            builder_.emit(ArrayLength{old_length,source});
            auto appended=lowerer_.lower_into(*append->args[0].value,*t.first);
            auto grown=builder_.fresh();
            builder_.emit(ArrayGrowMove{grown,source,t});
            builder_.emit(ArraySet{
                grown,old_length,appended,*t.first,
                static_cast<std::uint32_t>(s.span.start.line),
                static_cast<std::uint32_t>(s.span.start.column),true});
            builder_.emit(
                StoreLocal{local_name,grown,t,false,true});
            builder_.emit(Jump{done});

            builder_.enter(builder_.add_block(fallback));
            auto copied=lowerer_.lower_into(*n.value,t);
            builder_.emit(StoreLocal{local_name,copied,t});
            builder_.emit(Jump{done});

            builder_.enter(builder_.add_block(done));
            facts_.array_initialization.mark_full(target_name->name);
            return true;
        }
    }
    return false;
}

void AssignmentLowering::lower_plain_assignment(
    const Stmt& s, const AssignStmt& n, const Type& t) {
    const bool assigned_array_full =
        t.kind == TypeKind::Array &&
        facts_.array_initialization.fully_initialized(*n.value);
    // Where the order can be observed, the target's subexpressions come
    // first; the target is resolved from its root after the right-hand side
    // with their values, so the store reaches the storage as it is then.
    if(lowerer_.options().reorder_every_assignment ||
       assignment_order_.classify(n)!=AssignmentOrder::Tier::none){
        for(const auto* part:AssignmentOrder::target_subexpressions(*n.target))
            lowerer_.precompute(*part);
    }
    auto v=lowerer_.lower_into(*n.value,t);
    if(const auto* name=std::get_if<NameExpr>(&n.target->data)){
        if(const auto it=checked_.field_accesses.find(n.target.get());it!=checked_.field_accesses.end()){
            auto object=load_receiver(builder_,enclosing_class_);builder_.emit(FieldSet{object,it->second.index,v,t});
        } else if(scope_.is_source_reference(name->name)) {
            builder_.emit(StoreReference{scope_.source_reference(name->name),v,t});
        } else {
            const auto local_name=scope_.source_local(name->name);
            if(const auto* found=shapes_.tensor_extents(local_name)){
                shape_constraints_.emit_shaped_constraint(v,t.kind,*found,s.span);
            }
            if(const auto* found=shapes_.array_extents(local_name)){
                shape_constraints_.emit_array_constraints(v,t,*found,0,s.span);
            }
            builder_.emit(StoreLocal{local_name,v,t});
            facts_.record_local_store(name->name, t, *n.value, assigned_array_full);
        }
    }
    else if(const auto* member=std::get_if<MemberExpr>(&n.target->data)){
        auto object=lowerer_.lower(*member->base);const auto& info=checked_.field_accesses.at(n.target.get());
        builder_.emit(FieldSet{object,info.index,v,t});
    }
    else {
        const auto& ix=std::get<IndexExpr>(n.target->data);
        const auto base_type=type_of(checked_,*ix.base);
        const bool standard_collection_array =
            base_type.kind==TypeKind::Array &&
            enclosing_class_.is_standard_collection() &&
            checked_.field_accesses.contains(ix.base.get());
        const bool initialization_proven =
            base_type.kind==TypeKind::Array &&
            (facts_.array_initialization.fully_initialized(*ix.base) ||
             standard_collection_array);
        auto a=lowerer_.lower(*ix.base);
        if(base_type.kind==TypeKind::Tensor){
            std::vector<ValueId> indices;
            indices.reserve(ix.items.size());
            for(const auto& item:ix.items) indices.push_back(lowerer_.lower_int64(*item.index));
            builder_.emit(TensorSet{
                a,std::move(indices),v,t,
                static_cast<std::uint32_t>(n.target->span.start.line),
                static_cast<std::uint32_t>(n.target->span.start.column)});
        }else{
            // An element index failure reports the index operand.
            const auto& at=ix.items.front().index->span.start;
            auto i=lowerer_.lower_int64(*ix.items.front().index);
            if(base_type.kind==TypeKind::Bin) {
                builder_.emit(BinSet{
                    a,i,v,static_cast<std::uint32_t>(at.line),
                    static_cast<std::uint32_t>(at.column),
                    checked_.bounds_proven.contains(ix.items.front().index.get())});
                builder_.emit(Release{v,t});
            } else {
                const auto initialization_guard = initialization_proven
                    ? std::optional<ValueId>{}
                    : facts_.array_initialization.reference_guard(*ix.base);
                const bool bounds_proven =
                    checked_.bounds_proven.contains(
                        ix.items.front().index.get()) ||
                    standard_collection_array ||
                    facts_.array_bounds.proven_by_range_loop(
                        *ix.base, *ix.items.front().index);
                const auto bounds_guard = bounds_proven
                    ? std::optional<ValueId>{}
                    : facts_.array_bounds.reference_guard(
                        *ix.base, *ix.items.front().index);
                builder_.emit(ArraySet{
                    a,i,v,t,static_cast<std::uint32_t>(at.line),
                    static_cast<std::uint32_t>(at.column),
                    initialization_proven, bounds_proven,
                    initialization_guard, bounds_guard});
            }
        }
    }
}

} // namespace quidra::lowering
