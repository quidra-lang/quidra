#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "for_loop_constants.hpp"

namespace quidra::lowering {
namespace {
ValueId emit_constant(ir::FunctionBuilder& builder,const std::string& value,const Type& type) {
    auto out=builder.fresh();
    if(is_bare_integer(type)) builder.emit(ConstantExact{out,value,type});
    else builder.emit(ConstantInt{out,value,type});
    return out;
}
}
void ControlFlowLowering::lower_discard_sequence_for(const ForStmt& n) {
    const auto word=Type::simple(TypeKind::Int64);
    const auto* slice=std::get_if<IndexExpr>(&n.iterable->data);
    const bool sliced=slice&&slice->items.size()==1&&slice->items[0].slice;
    const auto& expression=sliced?*slice->base:*n.iterable;
    const auto type=type_of(checked_,expression);
    const auto source=lowerer_.lower(expression);
    auto length=builder_.fresh();
    if(type.kind==TypeKind::Bin) builder_.emit(BinLength{length,source});
    else builder_.emit(ArrayLength{length,source});
    // Only the entry length is observed. No element or source survives
    // evaluation of the bounds or entry into the body.
    lifetime_.release_temporary(expression,source);
    if(sliced) {
        const auto bounds=lowerer_.lower_slice_bounds(*n.iterable,slice->items[0]);
        const auto first=builder_.hidden("for.discard.slice_first");scope_.local_type(first)=word;
        builder_.emit(DeclareLocal{first,word,{}});
        auto address=builder_.fresh();builder_.emit(AddressLocal{address,first});
        auto count=builder_.fresh();builder_.emit(SliceLoopPlan{count,length,address,bounds,
            {static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column)}});
        length=count;
    }
    const auto counter=builder_.hidden("for.discard.counter");
    const auto limit=builder_.hidden("for.discard.length");
    scope_.local_type(counter)=scope_.local_type(limit)=word;
    builder_.emit(StoreLocal{counter,emit_constant(builder_,"0",word),word});
    builder_.emit(StoreLocal{limit,length,word});
    const auto cond=builder_.label("for.discard.condition"),body=builder_.label("for.discard.body"),
               advance=builder_.label("for.discard.advance"),done=builder_.label("for.discard.end"),
               leave=builder_.label("for.discard.break");
    builder_.emit(Jump{cond});builder_.enter(builder_.add_block(cond));
    auto index=builder_.fresh(),count=builder_.fresh(),test=builder_.fresh();
    builder_.emit(LoadLocal{index,counter,word});builder_.emit(LoadLocal{count,limit,word});
    builder_.emit(Binary{test,"<",index,count,word,Type::simple(TypeKind::Bool)});
    builder_.emit(Branch{test,body,done});builder_.enter(builder_.add_block(body));
    ValueId index_value=0;
    if(n.index) {
        index_value=lowerer_.bare_from_int64(index,Type::simple(TypeKind::Nat),false);
        scope_.bind_source_value(n.index->name,index_value);
        lifetime_.enter_loop_value(index_value,Type::simple(TypeKind::Nat));
    }
    loop_targets_.enter(advance,leave);lowerer_.lower_block(n.body);loop_targets_.leave();
    if(index_value) lifetime_.leave_loop_value();
    if(!current_block_ended(builder_)) builder_.emit(Jump{advance});
    builder_.enter(builder_.add_block(advance));
    if(index_value) release_owned(builder_,index_value,Type::simple(TypeKind::Nat));
    auto current=builder_.fresh(),next=builder_.fresh();builder_.emit(LoadLocal{current,counter,word});
    builder_.emit(Binary{next,"+",current,emit_constant(builder_,"1",word),word,word,0,0,true});
    builder_.emit(StoreLocal{counter,next,word});builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(leave));
    if(index_value) release_owned(builder_,index_value,Type::simple(TypeKind::Nat));
    builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(done));
}
void ControlFlowLowering::lower_discard_integer_range_for(const ForStmt& n) {
    const auto integer=Type::simple(TypeKind::Int),boolean=Type::simple(TypeKind::Bool);
    Type counter_type=integer;
    ValueId count{};
    std::vector<std::string> captured;
    const auto constant=[&](long long value) {
        return emit_constant(builder_,std::to_string(value),integer);
    };
    const auto binary=[&](const std::string& op,ValueId left,ValueId right,const Type& result) {
        auto out=builder_.fresh();builder_.emit(Binary{out,op,left,right,integer,result});
        if(result==integer) {
            auto name=builder_.hidden("for.discard.arithmetic");scope_.local_type(name)=integer;
            builder_.emit(StoreLocal{name,out,integer});captured.push_back(name);
            auto read=builder_.fresh();builder_.emit(LoadLocal{read,name,integer});return read;
        }
        return out;
    };
    if(const auto known=for_constants::integer_discard_count(n)) {
        const auto& text=known->text();
        if(text.size()<20 || (text.size()==20&&text<"18446744073709551616"))
            counter_type=Type::simple(TypeKind::Nat64);
        count=emit_constant(builder_,text,counter_type);
    } else {
        const auto capture=[&](const Expr& expression) {
            const auto source_type=type_of(checked_,expression);
            auto value=lowerer_.lower(expression);
            if(is_bare_integer(source_type)&&!lifetime_.expression_owns_result(expression))
                value=lifetime_.copy_value(value,source_type);
            auto converted=conversions_.convert(value,source_type,integer);
            auto name=builder_.hidden("for.discard.operand");scope_.local_type(name)=integer;
            builder_.emit(StoreLocal{name,converted,integer});captured.push_back(name);
            return name;
        };
        auto start_name=capture(*n.iterable),end_name=capture(*n.range->end);
        auto step_name=n.range->step?capture(*n.range->step):std::string{};
        const auto load=[&](const std::string& name) {
            auto value=builder_.fresh();builder_.emit(LoadLocal{value,name,integer});return value;
        };
        auto start=load(start_name),end=load(end_name);
        auto step=step_name.empty()?constant(n.range->descending?-1:1):load(step_name);
        builder_.emit(ForStepCheck{step,n.range->descending,
            {static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column)}});
        auto distance=binary("-",n.range->descending?start:end,n.range->descending?end:start,integer);
        auto magnitude=step;
        if(n.range->descending) {
            magnitude=builder_.fresh();builder_.emit(Unary{magnitude,"-",step,integer});
            auto name=builder_.hidden("for.discard.magnitude");scope_.local_type(name)=integer;
            builder_.emit(StoreLocal{name,magnitude,integer});captured.push_back(name);
        }
        const auto count_slot=builder_.hidden("for.discard.count");scope_.local_type(count_slot)=integer;
        builder_.emit(StoreLocal{count_slot,constant(0),integer});captured.push_back(count_slot);
        const auto calculate=builder_.label("for.discard.calculate"),ready=builder_.label("for.discard.ready");
        auto nonempty=binary(n.range->end_inclusive?">=":">",distance,constant(0),boolean);
        builder_.emit(Branch{nonempty,calculate,ready});builder_.enter(builder_.add_block(calculate));
        auto quotient=binary("/",distance,magnitude,integer);
        auto base=quotient;
        if(n.range->end_inclusive) base=binary("+",quotient,constant(1),integer);
        else {
            auto remainder=binary("%",distance,magnitude,integer);
            const auto extra=builder_.label("for.discard.extra"),finish=builder_.label("for.discard.ceiling");
            builder_.emit(StoreLocal{count_slot,lifetime_.copy_value(quotient,integer),integer});
            auto needed=binary("!=",remainder,constant(0),boolean);
            builder_.emit(Branch{needed,extra,finish});builder_.enter(builder_.add_block(extra));
            auto rounded=binary("+",quotient,constant(1),integer);
            builder_.emit(StoreLocal{count_slot,lifetime_.copy_value(rounded,integer),integer});builder_.emit(Jump{finish});
            builder_.enter(builder_.add_block(finish));base=load(count_slot);
        }
        if(!n.range->start_inclusive) {
            // The nonempty test guarantees an inclusive-start count >= 1.
            base=binary("-",base,constant(1),integer);
        }
        builder_.emit(StoreLocal{count_slot,lifetime_.copy_value(base,integer),integer});
        builder_.emit(Jump{ready});builder_.enter(builder_.add_block(ready));
        count=lifetime_.copy_value(load(count_slot),integer);
    }
    const auto counter=builder_.hidden("for.discard.counter"),limit=builder_.hidden("for.discard.limit");
    scope_.local_type(counter)=scope_.local_type(limit)=counter_type;
    builder_.emit(StoreLocal{counter,emit_constant(builder_,"0",counter_type),counter_type});
    builder_.emit(StoreLocal{limit,count,counter_type});
    const auto cond=builder_.label("for.discard.condition"),body=builder_.label("for.discard.body"),
               advance=builder_.label("for.discard.advance"),done=builder_.label("for.discard.end");
    builder_.emit(Jump{cond});builder_.enter(builder_.add_block(cond));
    auto position=builder_.fresh(),length=builder_.fresh(),test=builder_.fresh();
    builder_.emit(LoadLocal{position,counter,counter_type});builder_.emit(LoadLocal{length,limit,counter_type});
    builder_.emit(Binary{test,"<",position,length,counter_type,boolean});
    builder_.emit(Branch{test,body,done});builder_.enter(builder_.add_block(body));
    loop_targets_.enter(advance,done);lowerer_.lower_block(n.body);loop_targets_.leave();
    if(!current_block_ended(builder_)) builder_.emit(Jump{advance});
    builder_.enter(builder_.add_block(advance));
    auto value=builder_.fresh(),next=builder_.fresh();builder_.emit(LoadLocal{value,counter,counter_type});
    builder_.emit(Binary{next,"+",value,emit_constant(builder_,"1",counter_type),counter_type,counter_type,0,0,true});
    builder_.emit(StoreLocal{counter,next,counter_type});builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(done));
    if(owns_managed(counter_type)) {captured.push_back(counter);captured.push_back(limit);}
    for(const auto& name:captured) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,name,integer,true});release_owned(builder_,value,integer);
    }
}
}
