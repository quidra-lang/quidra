// Control flow lowering: if, while, for and match statements. A branch,
// loop body or match case starts from the scope and the fully initialized
// arrays its statement started with, and the paths that continue are
// joined after it. for loops run over a range, an array or a bin (after
// the text idioms that replace whole loops), with their break and continue
// targets; a match tries its idioms before testing the variant tag.
//
// A value loop over an array or a bin iterates the value at loop entry.
// When its body may write the iterated storage (storage_writes.hpp), the
// loop reads through a hidden source local that starts as the array itself,
// and takes its own reference or copy at the first statement that may write:
// a Retain before a statement that may only replace the array (the old
// buffer survives, and an append copies it instead of moving it), a Clone
// before one that may write its elements in place. The loop releases what
// it took at every exit: its end, break, and a return or `try` propagation
// inside it (LifetimeLowering's loop sources).
//
// A reference loop over a reference binding (a reference local or a `&`
// parameter) iterates the live array: it pins the array it enters with, so
// the array is never moved or finalized under the loop, and unpins it at
// every exit. After each statement of the body that may replace or resize
// the array through another binding (storage_writes.hpp), and before a
// break or continue inside such a statement, the loop checks that the
// binding still designates the array it entered with, at that length
// (IterationShapeCheck, FOR_ITERATION at the statement): replacing or
// resizing the array during the loop is an error, and no write-back reaches
// a released array.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {

void ControlFlowLowering::lower_if(const IfStmt& n) {
    auto cond=lowerer_.lower(*n.condition);
    const auto then_name=builder_.label("if.then"), else_name=builder_.label("if.else"), end_name=builder_.label("if.end");
    builder_.emit(Branch{cond,then_name,else_name});
    const auto before=scope_.snapshot_local_names();
    const auto before_full=facts_.array_initialization.full_arrays();

    auto& tb=builder_.add_block(then_name);
    builder_.enter(tb);
    facts_.array_initialization.set_full_arrays(before_full);
    for(const auto& x:n.then_body){ lowerer_.lower_statement(*x); if(current_block_ended(builder_)) break; }
    const bool then_continues=!current_block_ended(builder_);
    const auto then_full=facts_.array_initialization.full_arrays();
    if(then_continues) builder_.emit(Jump{end_name});
    scope_.restore(before);

    auto& eb=builder_.add_block(else_name);
    builder_.enter(eb);
    facts_.array_initialization.set_full_arrays(before_full);
    for(const auto& x:n.else_body){ lowerer_.lower_statement(*x); if(current_block_ended(builder_)) break; }
    const bool else_continues=!current_block_ended(builder_);
    const auto else_full=facts_.array_initialization.full_arrays();
    if(else_continues) builder_.emit(Jump{end_name});
    scope_.restore(before);

    if(then_continues && else_continues)
        facts_.array_initialization.set_full_arrays(intersect_full_arrays(then_full,else_full));
    else if(then_continues)
        facts_.array_initialization.set_full_arrays(then_full);
    else if(else_continues)
        facts_.array_initialization.set_full_arrays(else_full);
    else
        facts_.array_initialization.clear_full_arrays();

    auto& endb=builder_.add_block(end_name); builder_.enter(endb);
}

void ControlFlowLowering::lower_while(const WhileStmt& n) {
    const auto cond_name=builder_.label("while.cond"), body_name=builder_.label("while.body"), end_name=builder_.label("while.end");
    builder_.emit(Jump{cond_name});
    const auto before=scope_.snapshot_local_names();
    auto& cb=builder_.add_block(cond_name);
    builder_.enter(cb);
    auto c=lowerer_.lower(*n.condition);
    const auto condition_full=facts_.array_initialization.full_arrays();
    builder_.emit(Branch{c,body_name,end_name});

    auto& bb=builder_.add_block(body_name);
    builder_.enter(bb);
    scope_.restore(before);
    facts_.array_initialization.set_full_arrays(condition_full);
    loop_targets_.enter(cond_name, end_name);
    for(const auto& x:n.body){ lowerer_.lower_statement(*x); if(current_block_ended(builder_)) break; }
    loop_targets_.leave();
    const bool body_continues=!current_block_ended(builder_);
    const auto body_full=facts_.array_initialization.full_arrays();
    if(body_continues) builder_.emit(Jump{cond_name});

    scope_.restore(before);
    facts_.array_initialization.set_full_arrays(
        body_continues ? intersect_full_arrays(condition_full,body_full)
                       : condition_full);
    auto& eb=builder_.add_block(end_name); builder_.enter(eb);
}

void ControlFlowLowering::lower_for(const ForStmt& n) {
    const auto before=scope_.snapshot_local_names();
    const auto before_full=facts_.array_initialization.full_arrays();
    const auto loop_full_array=facts_.array_initialization.full_array_written_by_for(n);
    lower_for_loop(n);
    facts_.array_initialization.set_full_arrays(
        intersect_full_arrays(before_full,facts_.array_initialization.full_arrays()));
    if(loop_full_array) facts_.array_initialization.mark_full(*loop_full_array);
    scope_.restore(before);
}

void ControlFlowLowering::lower_for_loop(const ForStmt& n) {
    if (!lowerer_.initialization_flags().active()) {
        if (text_idioms_.lower_string_ascii_count_for(n)) return;
        if (text_idioms_.lower_string_split_for(n)) return;
    }
    const auto* range_call=std::get_if<CallExpr>(&n.iterable->data);
    const auto range_resolution=checked_.call_resolutions.find(n.iterable.get());
    if(range_call && range_resolution!=checked_.call_resolutions.end() &&
       range_resolution->second.kind==CallKind::Builtin &&
       range_resolution->second.builtin==BuiltinCallable::Range)
        return lower_range_for(n,*range_call);
    lower_sequence_for(n);
}

void ControlFlowLowering::lower_range_for(const ForStmt& n, const CallExpr& call) {
    // The bounds are ints; the counters are int64, and the loop variable
    // takes each counter value as an int.
    std::vector<ValueId> av; for(const auto& a:call.args) av.push_back(lowerer_.lower_int64(*a.value));
    ValueId start,end,step;
    if(av.size()==1){ start=builder_.const_int(0); end=av[0]; step=builder_.const_int(1); }
    else if(av.size()==2){ start=av[0]; end=av[1]; step=builder_.const_int(1); }
    else { start=av[0]; end=av[1]; step=av[2]; }
    builder_.emit(RangeCheckStep{step,static_cast<std::uint32_t>(n.iterable->span.start.line),static_cast<std::uint32_t>(n.iterable->span.start.column)});
    const auto i_name=builder_.hidden("range.i"), end_name=builder_.hidden("range.end"), step_name=builder_.hidden("range.step");
    scope_.local_type(i_name)=scope_.local_type(end_name)=scope_.local_type(step_name)=Type::simple(TypeKind::Int64); const auto iter_name=scope_.bind_source_local(builder_,n.name,Type::simple(TypeKind::Int));
    builder_.emit(StoreLocal{i_name,start,scope_.local_type(i_name)}); builder_.emit(StoreLocal{end_name,end,scope_.local_type(end_name)}); builder_.emit(StoreLocal{step_name,step,scope_.local_type(step_name)});
    const auto cond=builder_.label("for.cond"), body_name=builder_.label("for.body"),
               step_label=builder_.label("for.step"), done=builder_.label("for.end"); builder_.emit(Jump{cond});
    auto& cb=builder_.add_block(cond); builder_.enter(cb);
    auto si=builder_.fresh(), ii=builder_.fresh(), ee=builder_.fresh(); builder_.emit(LoadLocal{si,step_name,scope_.local_type(step_name)}); builder_.emit(LoadLocal{ii,i_name,scope_.local_type(i_name)}); builder_.emit(LoadLocal{ee,end_name,scope_.local_type(end_name)});
    auto zero=builder_.const_int(0), pos=builder_.fresh(), neg=builder_.fresh(), lt=builder_.fresh(), gt=builder_.fresh(), a=builder_.fresh(), b=builder_.fresh(), c=builder_.fresh();
    builder_.emit(Binary{pos,">",si,zero,scope_.local_type(step_name),Type::simple(TypeKind::Bool)}); builder_.emit(Binary{neg,"<",si,zero,scope_.local_type(step_name),Type::simple(TypeKind::Bool)});
    builder_.emit(Binary{lt,"<",ii,ee,scope_.local_type(i_name),Type::simple(TypeKind::Bool)}); builder_.emit(Binary{gt,">",ii,ee,scope_.local_type(i_name),Type::simple(TypeKind::Bool)});
    builder_.emit(Binary{a,"and",pos,lt,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)}); builder_.emit(Binary{b,"and",neg,gt,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)}); builder_.emit(Binary{c,"or",a,b,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)}); builder_.emit(Branch{c,body_name,done});
    const Expr* source_start = nullptr;
    const Expr* source_end = nullptr;
    const Expr* source_step = nullptr;
    if (call.args.size() == 1) {
        source_end = call.args[0].value.get();
    } else if (call.args.size() >= 2) {
        source_start = call.args[0].value.get();
        source_end = call.args[1].value.get();
        if (call.args.size() == 3)
            source_step = call.args[2].value.get();
    }
    // Every counter value lies between the bounds: inline when both are.
    const bool counter_inline = (!source_start || facts_.integer_ranges.inline_bounded(*source_start)) &&
                                facts_.integer_ranges.inline_bounded(*source_end);
    auto& bb=builder_.add_block(body_name); builder_.enter(bb); auto cur=builder_.fresh(); builder_.emit(LoadLocal{cur,i_name,scope_.local_type(i_name)});
    auto visible=lowerer_.bare_from_int64(cur,scope_.local_type(iter_name),counter_inline);
    builder_.emit(StoreLocal{iter_name,visible,scope_.local_type(iter_name)});
    const auto literal_is = [](const Expr* expression,
                               std::uint64_t expected) {
        if (!expression) return false;
        const auto* value =
            std::get_if<IntegerExpr>(&expression->data);
        return value && value->fits_u64 &&
               value->value == expected;
    };
    const bool range_bounds_trackable =
        source_end &&
        (!source_start || literal_is(source_start, 0)) &&
        (!source_step || literal_is(source_step, 1));
    if (range_bounds_trackable)
        facts_.array_bounds.push_range(
            ActiveRangeBound{n.name, source_end, &n.body});
    const bool unit_step = !source_step || literal_is(source_step, 1);
    if (unit_step) facts_.integer_ranges.enter_range_loop(n.name, n.body, source_start, *source_end);
    loop_targets_.enter(step_label, done);
    lowerer_.lower_block(n.body);
    loop_targets_.leave();
    if (unit_step) facts_.integer_ranges.leave_range_loop(n.name);
    if (range_bounds_trackable) facts_.array_bounds.pop_range();
    if(!current_block_ended(builder_)) builder_.emit(Jump{step_label});
    auto& sb=builder_.add_block(step_label); builder_.enter(sb);
    // For the canonical +1 range step, the body is entered only when
    // i < end. Therefore i + 1 <= end <= int.max, so the compiler-owned
    // induction increment cannot overflow. Arbitrary user-provided
    // steps keep ordinary checked arithmetic.
    const bool unit_positive_step =
        !source_step || literal_is(source_step, 1);
    auto x=builder_.fresh(), st=builder_.fresh(), nx=builder_.fresh(); builder_.emit(LoadLocal{x,i_name,scope_.local_type(i_name)}); builder_.emit(LoadLocal{st,step_name,scope_.local_type(step_name)}); builder_.emit(Binary{nx,"+",x,st,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64),0,0,unit_positive_step}); builder_.emit(StoreLocal{i_name,nx,scope_.local_type(i_name)}); builder_.emit(Jump{cond});
    auto& db=builder_.add_block(done); builder_.enter(db);
}

void ControlFlowLowering::lower_sequence_for(const ForStmt& n) {
    const auto array_type=type_of(checked_,*n.iterable);
    const bool iterable_initialization_proven =
        array_type.kind == TypeKind::Array &&
        facts_.array_initialization.fully_initialized(*n.iterable);
    auto array=lowerer_.lower(*n.iterable);
    const auto iterable_initialization_guard =
        array_type.kind == TypeKind::Array && !iterable_initialization_proven
            ? facts_.array_initialization.reference_guard(*n.iterable)
            : std::optional<ValueId>{};
    const auto item=array_type.kind==TypeKind::Bin?Type::simple(TypeKind::Bin):*array_type.first;
    const bool iterable_temporary=lifetime_.expression_owns_result(*n.iterable);
    // The writes of the body to the iterated storage, statement by
    // statement; a value loop over storage that some statement may write
    // reads through a copy-on-write source.
    using IterableWrite=StorageWrites::IterableWrite;
    const bool value_loop_over_storage=!n.writable && !iterable_temporary;
    const bool copy_at_entry=value_loop_over_storage &&
        lowerer_.options().copy_every_value_loop_at_entry;
    // A fixed array may be stored inside its parent's buffer, where only a
    // copy survives the parent: every write takes a copy.
    const bool fixed_array=array_type.kind==TypeKind::Array && array_type.length>=0;
    std::vector<IterableWrite> writes;
    if(value_loop_over_storage && !copy_at_entry){
        writes.reserve(n.body.size());
        for(const auto& statement:n.body){
            auto write=storage_writes_.statement_writes_iterable(*statement,*n.iterable);
            if(fixed_array && write==IterableWrite::replaces) write=IterableWrite::in_place;
            writes.push_back(write);
        }
    }
    const bool snapshot=copy_at_entry ||
        std::any_of(writes.begin(),writes.end(),[](IterableWrite write){
            return write!=IterableWrite::none;
        });
    std::optional<LifetimeLowering::LoopSource> source;
    std::string copied;
    if(snapshot){
        source=LifetimeLowering::LoopSource{
            builder_.hidden("for.source"),builder_.hidden("for.owned"),array_type};
        copied=builder_.hidden("for.copied");
        scope_.local_type(source->source)=array_type;
        scope_.local_type(source->owned)=scope_.local_type(copied)=Type::simple(TypeKind::Bool);
        auto initial=array;
        if(copy_at_entry){
            initial=builder_.fresh();
            builder_.emit(Clone{initial,array,array_type});
        }
        builder_.emit(StoreLocal{source->source,initial,array_type,true});
        auto taken=builder_.const_bool(copy_at_entry);
        builder_.emit(StoreLocal{source->owned,taken,Type::simple(TypeKind::Bool)});
        builder_.emit(StoreLocal{copied,taken,Type::simple(TypeKind::Bool)});
    }
    const auto idx_name=builder_.hidden("for.index"), len_name=builder_.hidden("for.length"); scope_.local_type(idx_name)=scope_.local_type(len_name)=Type::simple(TypeKind::Int64); const auto iter_name=scope_.bind_source_local(builder_,n.name,item);
    auto len=builder_.fresh();
    if(array_type.kind==TypeKind::Bin) builder_.emit(BinLength{len,array}); else builder_.emit(ArrayLength{len,array});
    auto zero=builder_.const_int(0); builder_.emit(StoreLocal{idx_name,zero,scope_.local_type(idx_name)}); builder_.emit(StoreLocal{len_name,len,scope_.local_type(len_name)});
    // A reference loop over a reference binding pins the array it enters
    // with, and lists the statements after which it checks the array.
    const bool pinned=n.writable && !iterable_temporary &&
        (array_type.kind==TypeKind::Array || array_type.kind==TypeKind::Bin) &&
        storage_writes_.names_reference_binding(*n.iterable);
    std::vector<bool> reshapes;
    if(pinned){
        builder_.emit(Pin{array});
        const bool every=lowerer_.options().check_every_reference_loop_statement;
        reshapes.reserve(n.body.size());
        for(const auto& statement:n.body)
            reshapes.push_back(every || storage_writes_.statement_may_reshape_iterable(*statement,*n.iterable));
    }
    const auto cond=builder_.label("for.cond"), body_name=builder_.label("for.body"),
               step_label=builder_.label("for.step"), break_label=builder_.label("for.break"),
               done=builder_.label("for.end"); builder_.emit(Jump{cond});
    auto& cb=builder_.add_block(cond); builder_.enter(cb); auto idx=builder_.fresh(), l=builder_.fresh(), cmp=builder_.fresh(); builder_.emit(LoadLocal{idx,idx_name,scope_.local_type(idx_name)}); builder_.emit(LoadLocal{l,len_name,scope_.local_type(len_name)}); builder_.emit(Binary{cmp,"<",idx,l,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)}); builder_.emit(Branch{cmp,body_name,done});
    auto& bb=builder_.add_block(body_name); builder_.enter(bb); auto ix=builder_.fresh(), element=builder_.fresh(); builder_.emit(LoadLocal{ix,idx_name,scope_.local_type(idx_name)});
    auto current=array;
    if(source){
        current=builder_.fresh();
        builder_.emit(LoadLocal{current,source->source,array_type});
    }
    if(array_type.kind==TypeKind::Bin) {
        builder_.emit(BinGet{element,current,ix,0,0,true});
    } else {
        builder_.emit(ArrayGet{
            element,current,ix,item,
            static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column),
            iterable_initialization_proven,true,iterable_initialization_guard});
    }
    const bool borrowed_iteration =
        array_type.kind == TypeKind::Array && !n.writable &&
        uses_shared_immutable_storage(item) &&
        !borrows_.block_mutates_parameter(n.body, n.name);
    if(array_type.kind!=TypeKind::Bin && !borrowed_iteration)
        element=lifetime_.copy_value(element,item);
    builder_.emit(
        StoreLocal{iter_name,element,item,borrowed_iteration});
    loop_targets_.enter(step_label, break_label);
    if(pinned){
        lifetime_.enter_loop_pin(array);
        lower_pinned_body(n,array,len,reshapes);
        lifetime_.leave_loop_pin();
    } else if(source){
        lifetime_.enter_loop_source(*source);
        // Before statement i, the retain or copy the statements up to the
        // next one lowered on its own may need: a text sequence that starts
        // at a binding may take the statements after it (the bindings up to
        // the next other statement, and at least three).
        const auto before=[&](std::size_t i){
            if(writes.empty()) return;
            auto write=writes[i];
            if(std::holds_alternative<BindingStmt>(n.body[i]->data)){
                for(std::size_t j=i+1;j<n.body.size();++j){
                    if(writes[j]==IterableWrite::in_place ||
                       (writes[j]==IterableWrite::replaces && write==IterableWrite::none))
                        write=writes[j];
                    if(j>=i+3 && !std::holds_alternative<BindingStmt>(n.body[j]->data)) break;
                }
            }
            if(write==IterableWrite::replaces) emit_source_retain(*source);
            else if(write==IterableWrite::in_place) emit_source_copy(*source,copied);
        };
        lowerer_.lower_block(n.body,before);
        lifetime_.leave_loop_source();
    } else {
        lowerer_.lower_block(n.body);
    }
    loop_targets_.leave();
    if(!current_block_ended(builder_)) builder_.emit(Jump{step_label});

    auto write_back=[&](){
        if(!n.writable)return;
        auto ix2=builder_.fresh(), val=builder_.fresh();
        builder_.emit(LoadLocal{ix2,idx_name,scope_.local_type(idx_name)});
        builder_.emit(LoadLocal{val,iter_name,item});
        if(array_type.kind==TypeKind::Bin) {
            builder_.emit(BinSet{array,ix2,val,0,0,true});
        } else {
            val=lifetime_.copy_value(val,item);
            builder_.emit(ArraySet{
            array,ix2,val,item,
            static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column),
            iterable_initialization_proven,true,iterable_initialization_guard});
        }
    };

    auto& sb=builder_.add_block(step_label); builder_.enter(sb); write_back();
    // The hidden sequence index increments only after idx < len succeeded,
    // hence idx + 1 <= len and cannot overflow an int. This proof concerns
    // compiler-owned loop state only; user arithmetic remains checked.
    auto old=builder_.fresh(); builder_.emit(LoadLocal{old,idx_name,scope_.local_type(idx_name)}); auto one=builder_.const_int(1), next=builder_.fresh(); builder_.emit(Binary{next,"+",old,one,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64),0,0,true}); builder_.emit(StoreLocal{idx_name,next,scope_.local_type(idx_name)}); builder_.emit(Jump{cond});
    auto& brb=builder_.add_block(break_label); builder_.enter(brb); write_back(); builder_.emit(Jump{done});
    auto& db=builder_.add_block(done); builder_.enter(db);
    if(pinned) builder_.emit(Unpin{array});
    if(source) lifetime_.release_loop_source(*source);
    if(iterable_temporary && requires_lifetime_management(array_type))
        builder_.emit(Release{array,array_type});
}

void ControlFlowLowering::lower_pinned_body(
    const ForStmt& n, ValueId entry_array, ValueId entry_length,
    const std::vector<bool>& reshapes) {
    const auto check=[&](const Stmt& statement){
        auto current=lowerer_.lower(*n.iterable);
        builder_.emit(IterationShapeCheck{
            current,entry_array,entry_length,
            static_cast<std::uint32_t>(statement.span.start.line),
            static_cast<std::uint32_t>(statement.span.start.column)});
    };
    // The statements before `checked` have been checked after, or need no
    // check; the check after a run of statements is located at the last one
    // that may replace or resize the array.
    std::size_t checked=0;
    const auto check_lowered=[&](std::size_t end){
        const Stmt* last=nullptr;
        for(std::size_t j=checked;j<end;++j)
            if(reshapes[j]) last=n.body[j].get();
        checked=end;
        if(last && !current_block_ended(builder_)) check(*last);
    };
    const auto before=[&](std::size_t i){
        check_lowered(i);
        // A break or continue lowered with statement i checks first when
        // the statements lowered with it may replace or resize the array: a
        // text sequence that starts at a binding may take the statements
        // after it (the bindings up to the next other statement, and at
        // least three).
        const Stmt* listed=reshapes[i]?n.body[i].get():nullptr;
        if(!listed && std::holds_alternative<BindingStmt>(n.body[i]->data)){
            for(std::size_t j=i+1;j<n.body.size() && !listed;++j){
                if(reshapes[j]) listed=n.body[j].get();
                if(j>=i+3 && !std::holds_alternative<BindingStmt>(n.body[j]->data)) break;
            }
        }
        if(listed) loop_targets_.set_before_jump([check,listed]{ check(*listed); });
        else loop_targets_.set_before_jump({});
    };
    lowerer_.lower_block(n.body,before);
    loop_targets_.set_before_jump({});
    check_lowered(n.body.size());
}

void ControlFlowLowering::emit_source_retain(const LifetimeLowering::LoopSource& source) {
    const auto flag=Type::simple(TypeKind::Bool);
    auto owned=builder_.fresh();
    builder_.emit(LoadLocal{owned,source.owned,flag});
    const auto take=builder_.label("for.source.retain");
    const auto done=builder_.label("for.source.retained");
    builder_.emit(Branch{owned,done,take});
    builder_.enter(builder_.add_block(take));
    auto value=builder_.fresh(), kept=builder_.fresh();
    builder_.emit(LoadLocal{value,source.source,source.type});
    builder_.emit(Retain{kept,value,source.type});
    builder_.emit(StoreLocal{source.source,kept,source.type,true});
    builder_.emit(StoreLocal{source.owned,builder_.const_bool(true),flag});
    builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(done));
}

void ControlFlowLowering::emit_source_copy(
    const LifetimeLowering::LoopSource& source, const std::string& copied) {
    const auto flag=Type::simple(TypeKind::Bool);
    auto has_copy=builder_.fresh();
    builder_.emit(LoadLocal{has_copy,copied,flag});
    const auto copy=builder_.label("for.source.copy");
    const auto release=builder_.label("for.source.release_retained");
    const auto store=builder_.label("for.source.store");
    const auto done=builder_.label("for.source.copied");
    builder_.emit(Branch{has_copy,done,copy});
    builder_.enter(builder_.add_block(copy));
    auto value=builder_.fresh(), cloned=builder_.fresh(), owned=builder_.fresh();
    builder_.emit(LoadLocal{value,source.source,source.type});
    builder_.emit(Clone{cloned,value,source.type});
    builder_.emit(LoadLocal{owned,source.owned,flag});
    builder_.emit(Branch{owned,release,store});
    builder_.enter(builder_.add_block(release));
    builder_.emit(Release{value,source.type});
    builder_.emit(Jump{store});
    builder_.enter(builder_.add_block(store));
    builder_.emit(StoreLocal{source.source,cloned,source.type,true});
    auto taken=builder_.const_bool(true);
    builder_.emit(StoreLocal{source.owned,taken,flag});
    builder_.emit(StoreLocal{copied,taken,flag});
    builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(done));
}

void ControlFlowLowering::lower_match(const MatchStmt& n) {
    if (!lowerer_.initialization_flags().active()) {
        if (text_idioms_.lower_utf8_array_match(n)) return;
        if (text_idioms_.lower_numeric_parse_match(n)) return;
        if (collection_idioms_.lower_standard_map_get_match(n)) return;
    }
    const bool container_owned=lifetime_.expression_owns_result(*n.value);
    auto container=lowerer_.lower(*n.value);
    if(container_owned){
        const auto owner_name=builder_.hidden("match.container");
        scope_.local_type(owner_name)=type_of(checked_,*n.value);
        builder_.emit(StoreLocal{owner_name,container,scope_.local_type(owner_name)});
    }
    auto tag=builder_.fresh();
    builder_.emit(VariantTag{tag,container});
    const auto mt=type_of(checked_,*n.value);
    auto done=builder_.label("match.end");
    const auto before=scope_.snapshot_local_names();
    const auto before_full=facts_.array_initialization.full_arrays();
    std::optional<std::unordered_set<std::string>> joined_full;
    for(auto& c:n.cases){
        const auto ct=checked_.case_types.at(&c);
        auto yes=builder_.label("match.case"),next=builder_.label("match.next");
        auto num=builder_.const_int(checked_.case_tags.at(&c)),test=builder_.fresh();
        builder_.emit(Binary{
            test,"==",tag,num,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
        builder_.emit(Branch{test,yes,next});
        builder_.enter(builder_.add_block(yes));
        scope_.restore(before);
        facts_.array_initialization.set_full_arrays(before_full);
        if(c.binder&&ct.kind!=TypeKind::Void&&ct.kind!=TypeKind::None){
            auto pv=builder_.fresh();
            builder_.emit(VariantPayload{pv,container,ct});
            const bool borrow=
                requires_value_clone(ct)&&!borrows_.block_mutates_parameter(c.body,*c.binder);
            if(!borrow)pv=conversions_.convert(pv,ct,ct,true);
            const auto binder_name=scope_.bind_source_local(builder_,*c.binder,ct);
            builder_.emit(StoreLocal{binder_name,pv,ct,borrow});
        }
        for(auto&x:c.body){lowerer_.lower_statement(*x);if(current_block_ended(builder_))break;}
        if(!current_block_ended(builder_)){
            if(joined_full)
                *joined_full=intersect_full_arrays(
                    *joined_full,facts_.array_initialization.full_arrays());
            else
                joined_full=facts_.array_initialization.full_arrays();
            builder_.emit(Jump{done});
        }
        builder_.enter(builder_.add_block(next));
        scope_.restore(before);
        facts_.array_initialization.set_full_arrays(before_full);
    }
    builder_.enter(builder_.add_block(done));
    if(joined_full) facts_.array_initialization.set_full_arrays(std::move(*joined_full));
    else facts_.array_initialization.clear_full_arrays();
}

ValueId ControlFlowLowering::lower_exit(const CallExpr& n) {
    auto v=lowerer_.lower_int64(*n.args[0].value);
    builder_.emit(Exit{v});
    return 0;
}

} // namespace quidra::lowering
