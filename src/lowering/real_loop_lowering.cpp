#include "for_loop_constants.hpp"
#include <array>
#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

namespace quidra::lowering {

void ControlFlowLowering::lower_real_header_range_for(const ForStmt& n) {
    const auto& header=*n.range;
    const auto type=type_of(checked_,*n.iterable);
    const auto integer=Type::simple(TypeKind::Int);
    const auto boolean=Type::simple(TypeKind::Bool);
    const auto array=Type::array(integer);
    const auto counter=builder_.hidden("for.real.counter");
    const auto count_name=builder_.hidden("for.real.count");
    const auto own=[&](ValueId value,const Type& at,const std::string& suffix) {
        auto name=builder_.hidden("for.real."+suffix);
        scope_.local_type(name)=at;
        builder_.emit(StoreLocal{name,value,at});
        return name;
    };
    const auto constant=[&](long long value,const Type& at) {
        auto result=builder_.fresh();
        if(is_bare_integer(at)) builder_.emit(ConstantExact{result,std::to_string(value),at});
        else builder_.emit(ConstantInt{result,std::to_string(value),at});
        return result;
    };
    const auto load=[&](const std::string& name,const Type& at) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,name,at});return value;
    };
    // Each entry operand is captured before evaluating the next one.
    auto start=lowerer_.lower(*n.iterable);
    if(type.kind==TypeKind::Real&&!lifetime_.expression_owns_result(*n.iterable))
        start=lifetime_.copy_value(start,type);
    const auto start_name=own(start,type,"start");
    auto end=lowerer_.lower(*header.end);
    if(type.kind==TypeKind::Real&&!lifetime_.expression_owns_result(*header.end))
        end=lifetime_.copy_value(end,type);
    const auto end_name=own(end,type,"end");
    auto step=lowerer_.lower(*header.step);
    if(type.kind==TypeKind::Real&&!lifetime_.expression_owns_result(*header.step))
        step=lifetime_.copy_value(step,type);
    const auto step_name=own(step,type,"step");
    start=load(start_name,type);end=load(end_name,type);step=load(step_name,type);
    ValueId count=builder_.fresh();
    std::string plan_name,numerator_name,denominator_name,stride_name,general_name,
                machine_flag,machine_numerator,machine_stride;
    if(type.kind==TypeKind::Real) {
        auto plan=builder_.fresh();
        std::optional<std::array<for_constants::Integer,6>> literal_plan;
        if(const auto a=for_constants::exact_literal(*n.iterable),
                      z=for_constants::exact_literal(*header.end),
                      b=for_constants::exact_literal(*header.step);a&&z&&b) {
            if(const auto total=for_constants::count(*a,*z,*b,header.descending,
                header.start_inclusive,header.end_inclusive)) {
                auto denominator=for_constants::divide_nonnegative(a->denominator,
                    for_constants::gcd(a->denominator,b->denominator)).first*b->denominator;
                auto first=a->numerator*for_constants::divide_nonnegative(denominator,a->denominator).first;
                const auto stride=b->numerator*for_constants::divide_nonnegative(denominator,b->denominator).first;
                if(!header.start_inclusive) first=first+stride;
                const auto after=first+*total*stride;
                const bool machine=for_constants::signed_word(first)&&for_constants::signed_word(stride)&&for_constants::signed_word(after);
                literal_plan=std::array<for_constants::Integer,6>{first,stride,denominator,*total,for_constants::Integer(0),for_constants::Integer(machine?1:0)};
            }
        }
        if(!literal_plan) {
            builder_.emit(ExactLoopPlan{plan,start,end,step,header.descending,
                header.start_inclusive,header.end_inclusive,
                {static_cast<std::uint32_t>(n.iterable->span.start.line),
                static_cast<std::uint32_t>(n.iterable->span.start.column)}});
            plan_name=own(plan,array,"plan");
        }
        const auto field=[&](int index) {
            auto value=builder_.fresh();
            if(literal_plan) builder_.emit(ConstantExact{value,(*literal_plan)[index].text(),integer});
            else builder_.emit(ArrayGet{value,plan,constant(index,Type::simple(TypeKind::Int64)),
                integer,{0,0},true,true});
            return value;
        };
        const auto own_field=[&](int index,const std::string& suffix) {
            auto value=field(index);
            if(!literal_plan) value=lifetime_.copy_value(value,integer);
            return own(value,integer,suffix);
        };
        numerator_name=own_field(0,"numerator");
        stride_name=own_field(1,"stride");
        denominator_name=own_field(2,"denominator");
        count=field(3);
        if(!literal_plan) count=lifetime_.copy_value(count,integer);
        general_name=own_field(4,"general");
        machine_flag=own_field(5,"machine_flag");
        const auto machine=Type::simple(TypeKind::Int64);
        machine_numerator=builder_.hidden("for.real.machine_numerator");
        machine_stride=builder_.hidden("for.real.machine_stride");
        scope_.local_type(machine_numerator)=scope_.local_type(machine_stride)=machine;
        const auto fast=builder_.label("for.real.machine_prepare"),ready=builder_.label("for.real.plan_ready");
        auto available=builder_.fresh();
        builder_.emit(Binary{available,"!=",load(machine_flag,integer),constant(0,integer),integer,boolean});
        builder_.emit(Branch{available,fast,ready});builder_.enter(builder_.add_block(fast));
        auto first=conversions_.convert(load(numerator_name,integer),integer,machine),
             stride=conversions_.convert(load(stride_name,integer),integer,machine);
        builder_.emit(StoreLocal{machine_numerator,first,machine});
        builder_.emit(StoreLocal{machine_stride,stride,machine});
        builder_.emit(Jump{ready});builder_.enter(builder_.add_block(ready));
    } else if(const auto a=for_constants::fixed_literal(*n.iterable,type,header.implicit_start_zero),
                         b=for_constants::fixed_literal(*header.end,type,header.implicit_end_zero),
                         c=for_constants::fixed_literal(*header.step,type);a&&b&&c) {
        const auto known=for_constants::count(*a,*b,*c,header.descending,
            header.start_inclusive,header.end_inclusive);
        builder_.emit(ConstantExact{count,known->text(),integer});
    } else builder_.emit(RealLoopCount{count,start,end,step,type,header.descending,
        header.start_inclusive,header.end_inclusive,
        {static_cast<std::uint32_t>(n.iterable->span.start.line),
        static_cast<std::uint32_t>(n.iterable->span.start.column)}});
    scope_.local_type(count_name)=scope_.local_type(counter)=integer;
    builder_.emit(StoreLocal{count_name,count,integer});
    builder_.emit(StoreLocal{counter,constant(0,integer),integer});
    ValueId native_fma=builder_.fresh();
#if defined(__APPLE__) && defined(__aarch64__)
    const bool native_half=type.kind==TypeKind::Real16;
#else
    const bool native_half=false;
#endif
    if(!lowerer_.options().exact_real_loop_values&&
       (native_half||type.kind==TypeKind::Real32||type.kind==TypeKind::Real64)) {
        auto maximum=builder_.fresh();
        builder_.emit(ConstantExact{maximum,native_half?
            (header.start_inclusive?"2049":"2048"):type.kind==TypeKind::Real32?
            (header.start_inclusive?"16777217":"16777216"):
            (header.start_inclusive?"9007199254740993":"9007199254740992"),integer});
        builder_.emit(Binary{native_fma,"<=",load(count_name,integer),maximum,integer,boolean});
    } else builder_.emit(ConstantBool{native_fma,false});
    const auto cond=builder_.label("for.real.condition");
    const auto body=builder_.label("for.real.body");
    const auto advance=builder_.label("for.real.advance");
    const auto done=builder_.label("for.real.end");
    const auto visible=n.discards()?std::string{}:
        n.declares?scope_.bind_source_local(builder_,n.name,type):scope_.source_local(n.name);
    if(n.declares&&!n.discards())
        builder_.emit(DeclareLocal{visible,type,n.name,
            static_cast<std::uint32_t>(n.iterable->span.start.line),
            static_cast<std::uint32_t>(n.iterable->span.start.column)});
    builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(cond));
    auto position=load(counter,integer),limit=load(count_name,integer),continues=builder_.fresh();
    builder_.emit(Binary{continues,"<",position,limit,integer,boolean});
    builder_.emit(Branch{continues,body,done});
    builder_.enter(builder_.add_block(body));
    std::string multiplier_name;
    auto multiplier=load(counter,integer);
    if(!header.start_inclusive) {
        auto shifted=builder_.fresh();
        builder_.emit(Binary{shifted,"+",multiplier,constant(1,integer),integer,integer});
        multiplier_name=own(shifted,integer,"multiplier");
        multiplier=load(multiplier_name,integer);
    }
    if(!n.discards()) {
        if(type.kind==TypeKind::Real) {
            if(!lowerer_.options().exact_real_loop_values)
                scope_.bind_scaled_real(n.name,{numerator_name,denominator_name,general_name,
                    start_name,step_name,counter,!header.start_inclusive,machine_numerator,machine_flag});
            // Existing variables keep the last produced value even if the
            // body never reads it. Declared variables materialize at a use.
            if(!n.declares||lowerer_.options().exact_real_loop_values) {
                auto value=builder_.fresh();
                builder_.emit(ExactLoopValue{value,multiplier,load(start_name,type),load(step_name,type)});
                builder_.emit(StoreLocal{visible,value,type});
                lowerer_.initialization_flags().for_iteration(n);
            }
        } else {
            auto value=builder_.fresh();
            builder_.emit(RealLoopValue{value,multiplier,load(start_name,type),load(step_name,type),type,native_fma});
            builder_.emit(StoreLocal{visible,value,type});
            lowerer_.initialization_flags().for_iteration(n);
        }
    }
    loop_targets_.enter(advance,done);
    lowerer_.lower_block(n.body);
    loop_targets_.leave();
    if(type.kind==TypeKind::Real&&!n.discards()) scope_.erase_scaled_real(n.name);
    if(!current_block_ended(builder_)) builder_.emit(Jump{advance});
    builder_.enter(builder_.add_block(advance));
    if(type.kind==TypeKind::Real&&!n.discards()) {
        auto available=builder_.fresh();
        builder_.emit(Binary{available,"!=",load(machine_flag,integer),constant(0,integer),integer,boolean});
        const auto fast=builder_.label("for.real.machine_advance"),wide=builder_.label("for.real.integer_advance"),
                   ready=builder_.label("for.real.advanced");
        builder_.emit(Branch{available,fast,wide});builder_.enter(builder_.add_block(fast));
        const auto machine=Type::simple(TypeKind::Int64);
        auto next=builder_.fresh();
        builder_.emit(Binary{next,"+",load(machine_numerator,machine),load(machine_stride,machine),machine,machine,0,0,true});
        builder_.emit(StoreLocal{machine_numerator,next,machine});
        builder_.emit(Jump{ready});builder_.enter(builder_.add_block(wide));
        next=builder_.fresh();
        builder_.emit(Binary{next,"+",load(numerator_name,integer),load(stride_name,integer),integer,integer});
        builder_.emit(StoreLocal{numerator_name,next,integer});
        builder_.emit(Jump{ready});builder_.enter(builder_.add_block(ready));
    }
    auto next=builder_.fresh();
    builder_.emit(Binary{next,"+",load(counter,integer),constant(1,integer),integer,integer});
    builder_.emit(StoreLocal{counter,next,integer});
    builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(done));
    if(n.declares&&!n.discards()&&owns_managed(type)) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,visible,type,true});release_owned(builder_,value,type);
    }
    // Clear owned hidden slots so both this exit and function cleanup are safe.
    for(const auto& name:{start_name,end_name,step_name,count_name,counter,
                         plan_name,numerator_name,denominator_name,stride_name,general_name,machine_flag,multiplier_name}) {
        if(name.empty()) continue;
        const auto& at=scope_.local_type_at(name);
        if(!owns_managed(at)) continue;
        auto value=builder_.fresh();
        builder_.emit(LoadLocal{value,name,at,true});
        release_owned(builder_,value,at);
    }
}

} // namespace quidra::lowering
