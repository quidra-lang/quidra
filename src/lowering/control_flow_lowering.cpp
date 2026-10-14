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
#include "lowering/syntax_queries.hpp"
#include "constant_integer_eval.hpp"
#include "lowering/lowerer.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {
namespace {
FullArrays stable_loop_full_arrays(
    const FullArrays& before, const std::vector<StmtPtr>& body,
    const CheckedProgram& checked, const Expr* condition = nullptr) {
    FullArrays stable;
    for (const auto& name : before) {
        const std::unordered_set<std::string> target{name};
        // A backedge reuses the emitted condition and body. Their entry
        // checks cannot rely on storage that another iteration may replace.
        if (!block_may_replace_array_reference(body, target, &checked) &&
            (!condition || !expression_may_replace_array_reference(
                               *condition, target, &checked))) {
            stable.insert(name);
        }
    }
    return stable;
}
} // namespace


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
    facts_.array_initialization.set_full_arrays(stable_loop_full_arrays(
        facts_.array_initialization.full_arrays(), n.body, checked_, n.condition.get()));
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
    std::optional<FullArrays> jump_full;
    loop_targets_.set_before_jump([&] {
        const auto& current = facts_.array_initialization.full_arrays();
        jump_full = jump_full ? intersect_full_arrays(*jump_full, current) : current;
    });
    for(const auto& x:n.body){ lowerer_.lower_statement(*x); if(current_block_ended(builder_)) break; }
    loop_targets_.leave();
    const bool body_continues=!current_block_ended(builder_);
    const auto body_full=facts_.array_initialization.full_arrays();
    if(body_continues) builder_.emit(Jump{cond_name});

    scope_.restore(before);
    auto exit_full = body_continues ? intersect_full_arrays(condition_full,body_full)
                                    : condition_full;
    // A break exits directly; a continue may reach the condition's false edge.
    // Neither may restore proofs discarded before its jump.
    if (jump_full) exit_full = intersect_full_arrays(exit_full, *jump_full);
    facts_.array_initialization.set_full_arrays(std::move(exit_full));
    auto& eb=builder_.add_block(end_name); builder_.enter(eb);
}

void ControlFlowLowering::lower_control_pattern(const Pattern& pattern,ValueId value,bool address) {
    const auto& expansion=checked_.control_patterns.at(&pattern);
    if(!address && !expansion.bindings.empty() && std::holds_alternative<AssignStmt>(expansion.bindings.front()->data)){lowerer_.lower_pattern_values(expansion,value);return;}
    if(address){auto name=scope_.bind_source_reference(builder_,expansion.subject,expansion.type);builder_.emit(DeclareReference{name,expansion.type});builder_.emit(BindReference{name,value});}
    else scope_.bind_source_value(expansion.subject,value);
    for(const auto& binding:expansion.bindings)lowerer_.lower_statement(*binding);
}

void ControlFlowLowering::lower_for(const ForStmt& n) {
    facts_.array_initialization.set_full_arrays(stable_loop_full_arrays(
        facts_.array_initialization.full_arrays(), n.body, checked_));
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
    if(n.parallel) return lowerer_.concurrency_.lower_parallel_for(n);
    if(!n.collections.empty()) return lower_multi_array_for(n);
    // Path enumeration shares a traversal plan but returns an ordinary array.
    if(type_of(checked_, *n.iterable).kind == TypeKind::Traversal) return lower_traversal_for(n);
    if (!n.pattern && !lowerer_.initialization_flags().active()) {
        if (text_idioms_.lower_string_ascii_count_for(n)) return;
        if (!n.range && text_idioms_.lower_string_split_for(n)) return;
    }
    if(n.range) return lower_header_range_for(n);
    if(n.pattern && checked_.control_patterns.at(&*n.pattern).bindings.empty())return lower_discard_sequence_for(n);
    lower_sequence_for(n);
}

void ControlFlowLowering::lower_header_range_for(const ForStmt& n) {
    const auto& header=*n.range;
    const auto type=type_of(checked_,*n.iterable);
    if(is_real(type)) return lower_real_header_range_for(n);
    if(n.discards()) return lower_discard_integer_range_for(n);
    const auto integer=Type::simple(TypeKind::Int);
    const auto boolean=Type::simple(TypeKind::Bool);
    const auto word=Type::simple(TypeKind::Nat64);
    const auto constant=[&](long long value,const Type& at) {
        auto result=builder_.fresh();
        if(is_bare_integer(at)) builder_.emit(ConstantExact{result,std::to_string(value),at});
        else builder_.emit(ConstantInt{result,std::to_string(value),at});
        return result;
    };
    const auto hold=[&](ValueId value,const Type& at,bool borrowed) {
        if(!is_bare_integer(at)) return value;
        const auto slot=builder_.hidden("for.range.operand");
        scope_.local_type(slot)=at;
        if(borrowed) value=lifetime_.copy_value(value,at);
        builder_.emit(StoreLocal{slot,value,at});
        auto out=builder_.fresh();
        builder_.emit(LoadLocal{out,slot,at});
        return out;
    };
    const auto binary=[&](const std::string& op,ValueId left,ValueId right,
                          const Type& operand,const Type& result,bool proven=false) {
        auto out=builder_.fresh();
        builder_.emit(Binary{out,op,left,right,operand,result,0,0,proven});
        return hold(out,result,false);
    };
    const auto convert=[&](ValueId value,const Type& from,const Type& to) {
        if(from==to) return value;
        auto out=builder_.fresh();
        builder_.emit(NumericConvert{out,value,from,to,false});
        return hold(out,to,false);
    };
    // Source order is observable: START, END, then the explicit STEP.
    auto start=lowerer_.lower(*n.iterable);
    start=hold(start,type,!lifetime_.expression_owns_result(*n.iterable));
    auto end=lowerer_.lower(*header.end);
    end=hold(end,type,!lifetime_.expression_owns_result(*header.end));
    auto step=header.step?lowerer_.lower(*header.step):constant(header.descending?-1:1,integer);
    const auto step_type=header.step?type_of(checked_,*header.step):integer;
    step=hold(step,step_type,!header.step || !lifetime_.expression_owns_result(*header.step));
    step=convert(step,step_type,integer);
    const auto step_origin=header.step?header.step->span.start:n.iterable->span.start;
    if(header.step && !constant_eval::integer(*header.step,nullptr))
        builder_.emit(ForStepCheck{step,header.descending,
            {static_cast<std::uint32_t>(step_origin.line),
            static_cast<std::uint32_t>(step_origin.column)}});
    auto magnitude=step;
    if(header.descending) {
        magnitude=builder_.fresh();
        builder_.emit(Unary{magnitude,"-",step,integer});
        magnitude=hold(magnitude,integer,false);
    }
    const auto literal_is=[](const Expr& expression,std::uint64_t value) {
        const auto* literal=std::get_if<IntegerExpr>(&expression.data);
        return literal && literal->fits_u64 && literal->value==value;
    };
    const bool canonical=!header.descending &&
        !header.end_inclusive && (!header.step || literal_is(*header.step,1));
    if(canonical) {
        const auto at=is_bare_integer(type)?integer:type;
        auto first=convert(start,type,at),limit=convert(end,type,at);
        const auto counter=builder_.hidden("for.range.counter");scope_.local_type(counter)=at;
        const auto visible=n.declares?scope_.bind_source_local(builder_,n.name,type):scope_.source_local(n.name);
        const auto condition=builder_.label("for.range.cond"),body=builder_.label("for.range.body");
        const auto advance=builder_.label("for.range.advance"),done=builder_.label("for.range.end");
        if(!header.start_inclusive) {
            const auto initialize=builder_.label("for.range.exclusive_start");
            builder_.emit(Branch{binary("<",first,limit,at,boolean),initialize,done});
            builder_.enter(builder_.add_block(initialize));
            first=binary("+",first,constant(1,at),at,at,true);
        }
        const auto raw=Type::simple(TypeKind::Int64);
        std::string inline_slot,raw_counter,raw_end,view;
        if(is_bare_integer(at)) {
            inline_slot=builder_.hidden("for.range.inline");scope_.local_type(inline_slot)=boolean;
            raw_counter=builder_.hidden("for.range.raw_counter");raw_end=builder_.hidden("for.range.raw_end");
            view=builder_.hidden("for.range.value");scope_.local_type(view)=at;
            scope_.local_type(raw_counter)=scope_.local_type(raw_end)=raw;
            auto x=builder_.fresh(),y=builder_.fresh();
            builder_.emit(Unary{x,"int.is_inline",first,boolean});builder_.emit(Unary{y,"int.is_inline",limit,boolean});
            auto guard=binary("and",x,y,boolean,boolean);builder_.emit(StoreLocal{inline_slot,guard,boolean});
            const auto fast=builder_.label("for.range.inline_entry"),slow=builder_.label("for.range.boxed_entry");
            builder_.emit(Branch{guard,fast,slow});builder_.enter(builder_.add_block(fast));
            for(const auto& pair:{std::pair{raw_counter,first},std::pair{raw_end,limit}}) {
                auto value=builder_.fresh();builder_.emit(NumericConvert{value,pair.second,at,raw,false,{0,0},true});
                builder_.emit(StoreLocal{pair.first,value,raw});
            }
            builder_.emit(Jump{condition});builder_.enter(builder_.add_block(slow));
            builder_.emit(StoreLocal{counter,lifetime_.copy_value(first,at),at});builder_.emit(Jump{condition});
        } else {
            builder_.emit(StoreLocal{counter,first,at});builder_.emit(Jump{condition});
        }
        builder_.enter(builder_.add_block(condition));
        if(is_bare_integer(at)) {
            const auto fast=builder_.label("for.range.inline_cond"),slow=builder_.label("for.range.boxed_cond");
            auto guard=builder_.fresh();builder_.emit(LoadLocal{guard,inline_slot,boolean});builder_.emit(Branch{guard,fast,slow});
            builder_.enter(builder_.add_block(fast));auto current=builder_.fresh(),end_value=builder_.fresh();
            builder_.emit(LoadLocal{current,raw_counter,raw});builder_.emit(LoadLocal{end_value,raw_end,raw});
            auto element=builder_.fresh();builder_.emit(NumericConvert{element,current,raw,at,false,{0,0},true});
            builder_.emit(StoreLocal{view,element,at,true});builder_.emit(Branch{binary("<",current,end_value,raw,boolean),body,done});
            builder_.enter(builder_.add_block(slow));current=builder_.fresh();builder_.emit(LoadLocal{current,counter,at});
            builder_.emit(StoreLocal{view,current,at,true});builder_.emit(Branch{binary("<",current,limit,at,boolean),body,done});
        } else {
            auto current=builder_.fresh();builder_.emit(LoadLocal{current,counter,at});
            builder_.emit(Branch{binary("<",current,limit,at,boolean),body,done});
        }
        builder_.enter(builder_.add_block(body));auto element=builder_.fresh();
        builder_.emit(LoadLocal{element,is_bare_integer(at)?view:counter,at});
        if(type.kind==TypeKind::Nat) {auto natural=builder_.fresh();builder_.emit(Unary{natural,"int.natural_view",element,type});element=natural;}
        if(!n.declares && is_bare_integer(type)) element=lifetime_.copy_value(element,type);
        builder_.emit(StoreLocal{visible,element,type,n.declares && is_bare_integer(type)});
        lowerer_.initialization_flags().for_iteration(n);
        const bool bounds_fact=literal_is(*n.iterable,0);
        if(bounds_fact) facts_.array_bounds.push_range(ActiveRangeBound{n.name,header.end.get(),&n.body});
        facts_.integer_ranges.enter_range_loop(n.name,n.body,n.iterable.get(),*header.end);
        loop_targets_.enter(advance,done);lowerer_.lower_block(n.body);loop_targets_.leave();
        facts_.integer_ranges.leave_range_loop(n.name);if(bounds_fact) facts_.array_bounds.pop_range();
        if(!current_block_ended(builder_)) builder_.emit(Jump{advance});
        builder_.enter(builder_.add_block(advance));
        if(is_bare_integer(at)) {
            const auto fast=builder_.label("for.range.inline_increment"),slow=builder_.label("for.range.boxed_increment");
            auto guard=builder_.fresh();builder_.emit(LoadLocal{guard,inline_slot,boolean});builder_.emit(Branch{guard,fast,slow});
            builder_.enter(builder_.add_block(fast));auto value=builder_.fresh();builder_.emit(LoadLocal{value,raw_counter,raw});
            builder_.emit(StoreLocal{raw_counter,binary("+",value,constant(1,raw),raw,raw,true),raw});builder_.emit(Jump{condition});
            builder_.enter(builder_.add_block(slow));value=builder_.fresh();builder_.emit(LoadLocal{value,counter,at});
            auto next=binary("+",value,constant(1,at),at,at);
            builder_.emit(StoreLocal{counter,lifetime_.copy_value(next,at),at});builder_.emit(Jump{condition});
        } else {
            auto value=builder_.fresh();builder_.emit(LoadLocal{value,counter,at});
            builder_.emit(StoreLocal{counter,binary("+",value,constant(1,at),at,at,true),at});builder_.emit(Jump{condition});
        }
        builder_.enter(builder_.add_block(done));return;
    }
    const auto counter_type=is_fixed_integer(type)?word:integer;
    const auto counter=builder_.hidden("for.range.counter");
    const auto last_name=builder_.hidden("for.range.last");
    const auto stride_name=builder_.hidden("for.range.stride");
    scope_.local_type(counter)=scope_.local_type(last_name)=scope_.local_type(stride_name)=counter_type;
    const auto visible=n.declares?scope_.bind_source_local(builder_,n.name,type):scope_.source_local(n.name);
    const auto prepare=builder_.label("for.range.prepare");
    const auto bounded=builder_.label("for.range.bounded_step");
    const auto single=builder_.label("for.range.single");
    const auto ready=builder_.label("for.range.ready");
    const auto body=builder_.label("for.range.body");
    const auto advance=builder_.label("for.range.advance");
    const auto increment=builder_.label("for.range.increment");
    const auto done=builder_.label("for.range.end");
    const bool equal_allowed=header.start_inclusive && header.end_inclusive;
    auto nonempty=binary(header.descending?(equal_allowed?">=":">"):(equal_allowed?"<=":"<"),
        start,end,type,boolean);
    builder_.emit(Branch{nonempty,prepare,done});
    builder_.enter(builder_.add_block(prepare));
    const auto coordinate=[&](ValueId value) {
        if(!is_fixed_integer(type)) return convert(value,type,integer);
        if(!is_signed_integer(type)) return convert(value,type,word);
        auto signed_word=convert(value,type,Type::simple(TypeKind::Int64));
        auto unsigned_word=convert(signed_word,Type::simple(TypeKind::Int64),word);
        auto sign=builder_.fresh();
        builder_.emit(ConstantInt{sign,"9223372036854775808",word});
        return binary("XOR",unsigned_word,sign,word,word);
    };
    // Signed coordinates flip the sign bit. Ordered endpoints then have an
    // unsigned distance even across the entire fixed-width signed domain.
    auto first=coordinate(start), stop=coordinate(end);
    const auto one=constant(1,counter_type);
    if(!header.end_inclusive)
        stop=binary(header.descending?"+":"-",stop,one,counter_type,counter_type,true);
    auto distance=binary("-",header.descending?first:stop,header.descending?stop:first,
        counter_type,counter_type,true);
    auto distance_integer=convert(distance,counter_type,integer);
    auto step_fits=binary("<=",magnitude,distance_integer,integer,boolean);
    builder_.emit(Branch{step_fits,bounded,header.start_inclusive?single:done});
    builder_.enter(builder_.add_block(single));
    builder_.emit(StoreLocal{counter,is_bare_integer(counter_type)?lifetime_.copy_value(first,counter_type):first,counter_type});
    builder_.emit(StoreLocal{last_name,is_bare_integer(counter_type)?lifetime_.copy_value(first,counter_type):first,counter_type});
    // A one-value loop exits before reading its stride.
    builder_.emit(StoreLocal{stride_name,one,counter_type});
    builder_.emit(Jump{ready});
    builder_.enter(builder_.add_block(bounded));
    auto stride=convert(magnitude,integer,counter_type);
    if(!header.start_inclusive) {
        first=binary(header.descending?"-":"+",first,stride,counter_type,counter_type,true);
        distance=binary("-",distance,stride,counter_type,counter_type,true);
    }
    auto remainder=binary("%",distance,stride,counter_type,counter_type);
    auto last=binary(header.descending?"+":"-",stop,remainder,counter_type,counter_type,true);
    builder_.emit(StoreLocal{counter,is_bare_integer(counter_type)?lifetime_.copy_value(first,counter_type):first,counter_type});
    builder_.emit(StoreLocal{last_name,is_bare_integer(counter_type)?lifetime_.copy_value(last,counter_type):last,counter_type});
    builder_.emit(StoreLocal{stride_name,is_bare_integer(counter_type)?lifetime_.copy_value(stride,counter_type):stride,counter_type});
    builder_.emit(Jump{ready});
    builder_.enter(builder_.add_block(ready));
    std::string inline_slot,raw_counter,raw_last,raw_stride,view;
    const auto raw=Type::simple(TypeKind::Int64);
    const auto entry=builder_.label("for.range.entry_value");
    if(is_bare_integer(counter_type)) {
        inline_slot=builder_.hidden("for.range.inline");scope_.local_type(inline_slot)=boolean;
        raw_counter=builder_.hidden("for.range.raw_counter");raw_last=builder_.hidden("for.range.raw_last");
        raw_stride=builder_.hidden("for.range.raw_stride");view=builder_.hidden("for.range.value");
        scope_.local_type(raw_counter)=scope_.local_type(raw_last)=scope_.local_type(raw_stride)=raw;
        scope_.local_type(view)=counter_type;
        auto guard=builder_.const_bool(true);
        for(const auto& slot:{counter,last_name,stride_name}) {
            auto value=builder_.fresh(),is_inline=builder_.fresh();builder_.emit(LoadLocal{value,slot,counter_type});
            builder_.emit(Unary{is_inline,"int.is_inline",value,boolean});guard=binary("and",guard,is_inline,boolean,boolean);
        }
        builder_.emit(StoreLocal{inline_slot,guard,boolean});
        const auto initialize=builder_.label("for.range.inline_entry");
        builder_.emit(Branch{guard,initialize,entry});builder_.enter(builder_.add_block(initialize));
        for(const auto& pair:{std::pair{raw_counter,counter},std::pair{raw_last,last_name},std::pair{raw_stride,stride_name}}) {
            auto word_value=builder_.fresh(),value=builder_.fresh();builder_.emit(LoadLocal{word_value,pair.second,counter_type});
            builder_.emit(NumericConvert{value,word_value,counter_type,raw,false,{0,0},true});builder_.emit(StoreLocal{pair.first,value,raw});
        }
        builder_.emit(Jump{entry});
    } else builder_.emit(Jump{entry});
    builder_.enter(builder_.add_block(entry));
    if(is_bare_integer(counter_type)) {
        const auto fast=builder_.label("for.range.inline_value"),slow=builder_.label("for.range.boxed_value");
        auto guard=builder_.fresh();builder_.emit(LoadLocal{guard,inline_slot,boolean});builder_.emit(Branch{guard,fast,slow});
        builder_.enter(builder_.add_block(fast));auto current=builder_.fresh(),element=builder_.fresh();
        builder_.emit(LoadLocal{current,raw_counter,raw});builder_.emit(NumericConvert{element,current,raw,counter_type,false,{0,0},true});
        builder_.emit(StoreLocal{view,element,counter_type,true});builder_.emit(Jump{body});
        builder_.enter(builder_.add_block(slow));current=builder_.fresh();builder_.emit(LoadLocal{current,counter,counter_type});
        builder_.emit(StoreLocal{view,current,counter_type,true});builder_.emit(Jump{body});
    } else builder_.emit(Jump{body});
    builder_.enter(builder_.add_block(body));auto current=builder_.fresh();
    builder_.emit(LoadLocal{current,is_bare_integer(counter_type)?view:counter,counter_type});
    auto element=current;
    if(is_fixed_integer(type)) {
        if(is_signed_integer(type)) {
            auto sign=builder_.fresh();
            builder_.emit(ConstantInt{sign,"9223372036854775808",word});
            element=binary("XOR",current,sign,word,word);
            element=convert(element,word,Type::simple(TypeKind::Int64));
            element=convert(element,Type::simple(TypeKind::Int64),type);
        } else element=convert(element,word,type);
    } else {
        // Bounds and the last lattice value already prove naturalness.
        // The int and nat word layouts coincide; this is a borrowed view.
        if(type.kind==TypeKind::Nat) {
            element=builder_.fresh();
            builder_.emit(Unary{element,"int.natural_view",current,type});
        }
    }
    if(!n.declares && is_bare_integer(type)) element=lifetime_.copy_value(element,type);
    builder_.emit(StoreLocal{visible,element,type,n.declares && is_bare_integer(type)});
    lowerer_.initialization_flags().for_iteration(n);

    const bool unit=!header.step || literal_is(*header.step,1);
    const bool forward_fact=!header.descending && header.start_inclusive &&
        !header.end_inclusive && literal_is(*n.iterable,0) && unit;
    const bool reverse_fact=header.descending && !header.start_inclusive &&
        header.end_inclusive && literal_is(*header.end,0) && !header.step;
    if(forward_fact || reverse_fact)
        facts_.array_bounds.push_range(ActiveRangeBound{n.name,
            reverse_fact?n.iterable.get():header.end.get(),&n.body});
    if(forward_fact)
        facts_.integer_ranges.enter_range_loop(n.name,n.body,n.iterable.get(),*header.end);
    loop_targets_.enter(advance,done);
    lowerer_.lower_block(n.body);
    loop_targets_.leave();
    if(forward_fact) facts_.integer_ranges.leave_range_loop(n.name);
    if(forward_fact || reverse_fact) facts_.array_bounds.pop_range();
    if(!current_block_ended(builder_)) builder_.emit(Jump{advance});
    builder_.enter(builder_.add_block(advance));
    if(is_bare_integer(counter_type)) {
        const auto fast=builder_.label("for.range.inline_advance"),slow=builder_.label("for.range.boxed_advance");
        const auto fast_increment=builder_.label("for.range.inline_increment");
        auto guard=builder_.fresh();builder_.emit(LoadLocal{guard,inline_slot,boolean});builder_.emit(Branch{guard,fast,slow});
        builder_.enter(builder_.add_block(fast));auto value=builder_.fresh(),last_value=builder_.fresh();
        builder_.emit(LoadLocal{value,raw_counter,raw});builder_.emit(LoadLocal{last_value,raw_last,raw});
        builder_.emit(Branch{binary("==",value,last_value,raw,boolean),done,fast_increment});
        builder_.enter(builder_.add_block(fast_increment));auto amount=builder_.fresh();builder_.emit(LoadLocal{amount,raw_stride,raw});
        builder_.emit(StoreLocal{raw_counter,binary(header.descending?"-":"+",value,amount,raw,raw,true),raw});builder_.emit(Jump{entry});
        builder_.enter(builder_.add_block(slow));
    }
    auto value=builder_.fresh(),last_value=builder_.fresh();
    builder_.emit(LoadLocal{value,counter,counter_type});builder_.emit(LoadLocal{last_value,last_name,counter_type});
    builder_.emit(Branch{binary("==",value,last_value,counter_type,boolean),done,increment});
    builder_.enter(builder_.add_block(increment));auto amount=builder_.fresh();builder_.emit(LoadLocal{amount,stride_name,counter_type});
    auto next=binary(header.descending?"-":"+",value,amount,counter_type,counter_type,true);
    builder_.emit(StoreLocal{counter,is_bare_integer(counter_type)?lifetime_.copy_value(next,counter_type):next,counter_type});builder_.emit(Jump{entry});
    builder_.enter(builder_.add_block(done));
}

void ControlFlowLowering::lower_sequence_for(const ForStmt& n) {
    if(n.discards()) return lower_discard_sequence_for(n);
    const auto* slice=std::get_if<IndexExpr>(&n.iterable->data);
    bool direct_slice=!n.writable && slice && slice->items.size()==1 && slice->items[0].slice;
    if(direct_slice) {
        const auto& bounds=slice->items[0];
        for(const auto* operand:{bounds.start.get(),bounds.stop.get(),bounds.step.get()})
            if(operand && expression_runs_code(*operand)) direct_slice=false;
        for(const auto& statement:n.body)
            if(storage_writes_.statement_writes_iterable(*statement,*slice->base)!=StorageWrites::IterableWrite::none)
                direct_slice=false;
    }
    const auto& source_expression=direct_slice?*slice->base:*n.iterable;
    const auto array_type=type_of(checked_,source_expression);
    const bool iterable_initialization_proven =
        array_type.kind == TypeKind::Array &&
        facts_.array_initialization.fully_initialized(source_expression);
    std::vector<const Expr*> captured;
    if(n.writable) {
        captured=AssignmentOrder::target_subexpressions(*n.iterable);
        for(const auto* operand:captured) lowerer_.capture_loop_operand(*operand);
    }
    auto array=lowerer_.lower(source_expression);
    std::optional<SliceBounds> slice_bounds;
    if(direct_slice) slice_bounds=lowerer_.lower_slice_bounds(*n.iterable,slice->items[0]);
    const auto iterable_initialization_guard =
        array_type.kind == TypeKind::Array && !iterable_initialization_proven
            ? facts_.array_initialization.reference_guard(source_expression)
            : std::optional<ValueId>{};
    const auto item=array_type.kind==TypeKind::Bin?Type::simple(TypeKind::Bin):*array_type.first;
    const bool iterable_temporary=lifetime_.expression_owns_result(source_expression);
    // The writes of the body to the iterated storage, statement by
    // statement; a value loop over storage that some statement may write
    // reads through a copy-on-write source.
    using IterableWrite=StorageWrites::IterableWrite;
    const bool value_loop_over_storage=!n.writable && !iterable_temporary && !direct_slice;
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
    if(snapshot || iterable_temporary){
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
        auto taken=builder_.const_bool(copy_at_entry || iterable_temporary);
        builder_.emit(StoreLocal{source->owned,taken,Type::simple(TypeKind::Bool)});
        builder_.emit(StoreLocal{copied,taken,Type::simple(TypeKind::Bool)});
    }
    const auto idx_name=builder_.hidden("for.index"), len_name=builder_.hidden("for.length"); scope_.local_type(idx_name)=scope_.local_type(len_name)=Type::simple(TypeKind::Int64); const auto iter_name=n.pattern?std::string{}:n.writable && array_type.kind==TypeKind::Array
        ? scope_.bind_source_reference(builder_,n.name,item)
        : n.declares?scope_.bind_source_local(builder_,n.name,item):scope_.source_local(n.name);
    if(!n.pattern && n.writable && array_type.kind==TypeKind::Array)
        builder_.emit(DeclareReference{iter_name,item});
    auto len=builder_.fresh();
    if(array_type.kind==TypeKind::Bin) builder_.emit(BinLength{len,array}); else builder_.emit(ArrayLength{len,array});
    ValueId slice_first=0,slice_stride=0;
    if(direct_slice) {
        const auto first_slot=builder_.hidden("for.slice.first");scope_.local_type(first_slot)=Type::simple(TypeKind::Int64);
        builder_.emit(DeclareLocal{first_slot,Type::simple(TypeKind::Int64),{}});
        auto address=builder_.fresh();builder_.emit(AddressLocal{address,first_slot});
        auto count=builder_.fresh();
        builder_.emit(SliceLoopPlan{count,len,address,*slice_bounds,
            {static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column)}});
        len=count;slice_first=builder_.fresh();builder_.emit(LoadLocal{slice_first,first_slot,Type::simple(TypeKind::Int64)});
        if (slice_bounds->step) slice_stride = *slice_bounds->step;
        else if (slice_bounds->direction == SliceDirection::by_bounds) {
            // Reuse the already-evaluated endpoints. A direct slice loop
            // traverses the same progression as materializing the slice.
            const auto integer = Type::simple(TypeKind::Int64);
            const auto descending = builder_.fresh();
            builder_.emit(Binary{descending, ">", *slice_bounds->start, *slice_bounds->end,
                integer, Type::simple(TypeKind::Bool)});
            const auto down = builder_.label("for.slice.descending");
            const auto up = builder_.label("for.slice.ascending");
            const auto ready = builder_.label("for.slice.stride");
            const auto stride = builder_.hidden("for.slice.stride");
            scope_.local_type(stride) = integer;
            builder_.emit(Branch{descending,down,up});
            builder_.enter(builder_.add_block(down));
            builder_.emit(StoreLocal{stride,builder_.const_int(-1),integer});
            builder_.emit(Jump{ready});
            builder_.enter(builder_.add_block(up));
            builder_.emit(StoreLocal{stride,builder_.const_int(1),integer});
            builder_.emit(Jump{ready});
            builder_.enter(builder_.add_block(ready));
            slice_stride = builder_.fresh();
            builder_.emit(LoadLocal{slice_stride,stride,integer});
        } else slice_stride = builder_.const_int(slice_bounds->direction==SliceDirection::descending?-1:1);
    }
    auto zero=builder_.const_int(0); builder_.emit(StoreLocal{idx_name,zero,scope_.local_type(idx_name)}); builder_.emit(StoreLocal{len_name,len,scope_.local_type(len_name)});
    // A reference loop over a reference binding pins the array it enters
    // with, and lists the statements after which it checks the array.
    std::vector<bool> reshapes;
    if(n.writable) {
        const bool every=lowerer_.options().check_every_reference_loop_statement;
        reshapes.reserve(n.body.size());
        for(const auto& statement:n.body)
            reshapes.push_back(every || storage_writes_.statement_may_reshape_iterable(*statement,*n.iterable));
    }
    const bool pinned=n.writable && !iterable_temporary &&
        (storage_writes_.names_reference_binding(*n.iterable) ||
         std::any_of(reshapes.begin(),reshapes.end(),[](bool checks){return checks;}));
    if(pinned) builder_.emit(Pin{array});
    std::vector<PathGuard> prefixes;
    if(pinned) {
        const auto visit=[&](const auto& self,const Expr& place)->void {
            const Expr* base=nullptr;
            if(const auto* field=std::get_if<MemberExpr>(&place.data)) base=field->base.get();
            if(const auto* element=std::get_if<IndexExpr>(&place.data)) base=element->base.get();
            if(!base) return;
            self(self,*base);
            const auto type=type_of(checked_,*base);
            if(type.kind==TypeKind::Array || type.kind==TypeKind::Bin) {
                auto value=lowerer_.lower(*base),length=builder_.fresh();
                if(type.kind==TypeKind::Bin) builder_.emit(BinLength{length,value});
                else builder_.emit(ArrayLength{length,value});
                prefixes.push_back(PathGuard{base,value,length});
            }
        };
        visit(visit,*n.iterable);
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
    auto physical_index=ix;
    if(direct_slice) {
        auto offset=builder_.fresh();physical_index=builder_.fresh();
        const auto integer=Type::simple(TypeKind::Int64);
        builder_.emit(Binary{offset,"*",ix,slice_stride,integer,integer,0,0,true});
        builder_.emit(Binary{physical_index,"+",slice_first,offset,integer,integer,0,0,true});
    }
    if(n.writable) {
        if(array_type.kind==TypeKind::Bin) {
            scope_.bind_bit_place(n.name,current,ix);
        } else {
            auto address=builder_.fresh();
            builder_.emit(AddressElement{address,current,ix,array_type,item,false,{0,0},true});
            if(n.pattern){auto initialized=builder_.fresh();builder_.emit(ArrayGet{initialized,current,ix,item,{static_cast<std::uint32_t>(n.iterable->span.start.line),static_cast<std::uint32_t>(n.iterable->span.start.column)},iterable_initialization_proven,true,iterable_initialization_guard});lower_control_pattern(*n.pattern,address,true);}
            else builder_.emit(BindReference{iter_name,address});
        }
    } else {
    if(array_type.kind==TypeKind::Bin) {
        builder_.emit(BinGet{element,current,physical_index,{0,0},true});
    } else {
        builder_.emit(ArrayGet{
            element,current,physical_index,item,
            {static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column)},
            iterable_initialization_proven,true,iterable_initialization_guard});
    }
    if(n.pattern)lower_control_pattern(*n.pattern,element,false);
    else {
    const bool borrowed_iteration =
        array_type.kind == TypeKind::Array && !n.writable && n.declares &&
        uses_shared_immutable_storage(item) &&
        !borrows_.block_mutates_parameter(n.body, n.name);
    if(array_type.kind!=TypeKind::Bin && !borrowed_iteration)
        element=lifetime_.copy_value(element,item);
    builder_.emit(
        StoreLocal{iter_name,element,item,borrowed_iteration});
    lowerer_.initialization_flags().for_iteration(n);
    }
    }
    ValueId index_value=0;
    if(n.index && std::any_of(n.body.begin(),n.body.end(),[&](const StmtPtr& statement){return statement_mentions_name(*statement,n.index->name);})) {
        index_value=lowerer_.bare_from_int64(ix,Type::simple(TypeKind::Nat),false);
        scope_.bind_source_value(n.index->name,index_value);
        lifetime_.enter_loop_value(index_value,Type::simple(TypeKind::Nat));
    }
    loop_targets_.enter(step_label, break_label);
    std::size_t pattern_references=0;
    if(n.pattern && n.writable){visit_pattern(*n.pattern,[&](const Pattern& p){if(p.kind==Pattern::Kind::Declare){lifetime_.enter_loop_reference(scope_.source_reference(p.name));++pattern_references;}});}
    else if(n.writable && array_type.kind==TypeKind::Array)lifetime_.enter_loop_reference(iter_name);
    if(pinned){
        lifetime_.enter_loop_pin(array);
        lower_pinned_body(n,array,len,reshapes,prefixes);
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
    if(n.pattern){for(std::size_t i=0;i<pattern_references;++i)lifetime_.leave_loop_reference();}
    else if(n.writable && array_type.kind==TypeKind::Array)lifetime_.leave_loop_reference();
    if(index_value) lifetime_.leave_loop_value();
    loop_targets_.leave();
    if(!current_block_ended(builder_)) builder_.emit(Jump{step_label});

    auto& sb=builder_.add_block(step_label); builder_.enter(sb);
    if(index_value) release_owned(builder_,index_value,Type::simple(TypeKind::Nat));
    // The hidden sequence index increments only after idx < len succeeded,
    // hence idx + 1 <= len and cannot overflow an int. This proof concerns
    // compiler-owned loop state only; user arithmetic remains checked.
    auto old=builder_.fresh(); builder_.emit(LoadLocal{old,idx_name,scope_.local_type(idx_name)}); auto one=builder_.const_int(1), next=builder_.fresh(); builder_.emit(Binary{next,"+",old,one,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64),0,0,true}); builder_.emit(StoreLocal{idx_name,next,scope_.local_type(idx_name)}); builder_.emit(Jump{cond});
    auto& brb=builder_.add_block(break_label); builder_.enter(brb);
    if(index_value) release_owned(builder_,index_value,Type::simple(TypeKind::Nat));
    builder_.emit(Jump{done});
    auto& db=builder_.add_block(done); builder_.enter(db);
    if(n.pattern && n.writable)visit_pattern(*n.pattern,[&](const Pattern& p){if(p.kind==Pattern::Kind::Declare)builder_.emit(BindReference{scope_.source_reference(p.name),0});});
    else if(n.writable && array_type.kind==TypeKind::Array)builder_.emit(BindReference{iter_name,0});
    if(n.pattern && n.writable)builder_.emit(BindReference{scope_.source_reference(checked_.control_patterns.at(&*n.pattern).subject),0});
    if(pinned) builder_.emit(Unpin{array});
    if(source) lifetime_.release_loop_source(*source);
    if(iterable_temporary && !source && owns_managed(array_type))
        release_owned(builder_,array,array_type);
    for(const auto* operand:captured) lowerer_.forget_loop_operand(*operand);
}

void ControlFlowLowering::lower_pinned_body(
    const ForStmt& n, ValueId entry_array, ValueId entry_length,
    const std::vector<bool>& reshapes, const std::vector<PathGuard>& prefixes) {
    const auto check=[&](const Stmt& statement){
        // Validate outer arrays before re-resolving a child. A replaced or
        // shortened matrix reports FOR_ITERATION, never a secondary index
        // failure while walking its captured element index.
        for(const auto& prefix:prefixes) {
            auto current=lowerer_.lower(*prefix.expression);
            builder_.emit(IterationShapeCheck{current,prefix.array,prefix.length,
                {static_cast<std::uint32_t>(statement.span.start.line),
                static_cast<std::uint32_t>(statement.span.start.column)}});
        }
        auto current=lowerer_.lower(*n.iterable);
        builder_.emit(IterationShapeCheck{
            current,entry_array,entry_length,
            {static_cast<std::uint32_t>(statement.span.start.line),
            static_cast<std::uint32_t>(statement.span.start.column)}});
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
    release_owned(builder_,value,source.type);
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
    if (std::none_of(n.cases.begin(),n.cases.end(),[](const MatchCase& c){return c.pattern.has_value();}) && !lowerer_.initialization_flags().active()) {
        if (text_idioms_.lower_utf8_array_match(n)) return;
        if (text_idioms_.lower_numeric_parse_match(n)) return;
        if (collection_idioms_.lower_standard_map_get_match(n)) return;
    }
    const bool container_owned=lifetime_.expression_owns_result(*n.value);
    const auto hop_call=container_owned?lowerer_.error_hop_call(*n.value):std::nullopt;
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
        // A brace shape (L17): the case's tests, which go on to the next
        // case when one fails, and its shape variables.
        if(c.type.brace_shape && ct.kind==TypeKind::Tensor){
            if(const auto plan=checked_.shape_plans.find(&c.type);
               plan!=checked_.shape_plans.end()){
                auto payload=builder_.fresh();
                builder_.emit(VariantPayload{payload,container,ct});
                shape_constraints_.lower_case_shape(plan->second,payload,ct.kind,next,c.span);
            }
        }
        if(c.pattern){auto payload=builder_.fresh();builder_.emit(VariantPayload{payload,container,ct});lower_control_pattern(*c.pattern,payload,false);}
        else if(c.binder&&ct.kind==TypeKind::Error&&hop_call){
            // The error leaves the call into this case: a hop, which takes
            // it out of the cell the hidden container still releases.
            auto pv=lowerer_.error_hop(container,mt,*hop_call,abi::ErrorHopKind::match);
            const auto binder_name=scope_.bind_source_local(builder_,*c.binder,ct);
            builder_.emit(StoreLocal{binder_name,pv,ct,false});
        }else if(c.binder&&ct.kind!=TypeKind::Void&&ct.kind!=TypeKind::None){
            auto pv=builder_.fresh();
            builder_.emit(VariantPayload{pv,container,ct});
            const bool borrow=
                clone_needing(ct)&&!borrows_.block_mutates_parameter(c.body,*c.binder);
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
