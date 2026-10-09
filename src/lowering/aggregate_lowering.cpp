// Aggregate lowering: the methods of arrays (sorted, join, append, concat)
// and the builtins array(count, fill) and len(). A fixed-length receiver is
// first converted to a dynamic array; append, concat and a fill that is not
// zero write the elements in counted loops.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <cmath>
#include <optional>
#include <string>

namespace quidra::lowering {

std::optional<ValueId> AggregateLowering::try_array_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::Array && n.method=="sorted"){
        const bool receiver_owned=lifetime_.expression_owns_result(*n.receiver);
        auto source=lowerer_.lower(*n.receiver);
        auto source_type=receiver_type;
        bool source_owned=receiver_owned;
        if(receiver_type.length>=0){
            const auto dynamic_type=Type::array(*receiver_type.first);
            auto converted=conversions_.convert(source,receiver_type,dynamic_type);
            if(receiver_owned) builder_.emit(Release{source,receiver_type});
            source=converted;
            source_type=dynamic_type;
            source_owned=true;
        }
        auto out=builder_.fresh();
        builder_.emit(ArraySorted{
            out,source,source_type,
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)});
        if(source_owned) builder_.emit(Release{source,source_type});
        return out;
    }
    if(receiver_type.kind==TypeKind::Array && n.method=="join"){
        const bool receiver_owned=lifetime_.expression_owns_result(*n.receiver);
        auto source=lowerer_.lower(*n.receiver);
        auto source_type=receiver_type;
        bool source_owned=receiver_owned;
        if(receiver_type.length>=0){
            const auto dynamic_type=Type::array(*receiver_type.first);
            auto converted=conversions_.convert(source,receiver_type,dynamic_type);
            if(receiver_owned) builder_.emit(Release{source,receiver_type});
            source=converted;
            source_type=dynamic_type;
            source_owned=true;
        }
        auto separator=lowerer_.lower(*n.args[0].value);
        auto out=builder_.fresh();
        builder_.emit(StringJoin{
            out,source,separator,
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)});
        lifetime_.release_temporary(*n.args[0].value,separator);
        if(source_owned) builder_.emit(Release{source,source_type});
        return out;
    }
    if(receiver_type.kind==TypeKind::Array&&(n.method=="append"||n.method=="concat")){
        const bool source_owned=lifetime_.expression_owns_result(*n.receiver);
        auto source=lowerer_.lower(*n.receiver),source_length=builder_.fresh();
        builder_.emit(ArrayLength{source_length,source});
        const auto element_type=*receiver_type.first;
        const auto result_type=Type::array(element_type);
        auto copy_into=[&](ValueId from,ValueId length,ValueId destination,ValueId offset,const std::string& prefix){
            const auto index_name=builder_.hidden(prefix+".index");scope_.local_type(index_name)=Type::simple(TypeKind::Int64);
            const auto zero=builder_.const_int(0);builder_.emit(StoreLocal{index_name,zero,scope_.local_type(index_name)});
            const auto cond=builder_.label(prefix+".cond"),body=builder_.label(prefix+".body"),done=builder_.label(prefix+".done");
            builder_.emit(Jump{cond});builder_.enter(builder_.add_block(cond));
            auto index=builder_.fresh(),more=builder_.fresh();builder_.emit(LoadLocal{index,index_name,scope_.local_type(index_name)});
            builder_.emit(Binary{more,"<",index,length,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
            builder_.emit(Branch{more,body,done});builder_.enter(builder_.add_block(body));
            auto item=builder_.fresh();builder_.emit(ArrayGet{item,from,index,element_type});item=lifetime_.copy_value(item,element_type);
            auto destination_index=builder_.fresh();builder_.emit(Binary{destination_index,"+",offset,index,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64)});
            builder_.emit(ArraySet{destination,destination_index,item,element_type});
            const auto one=builder_.const_int(1);auto next=builder_.fresh();builder_.emit(Binary{next,"+",index,one,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64)});
            builder_.emit(StoreLocal{index_name,next,scope_.local_type(index_name)});builder_.emit(Jump{cond});builder_.enter(builder_.add_block(done));
        };
        if(n.method=="append"){
            const auto one=builder_.const_int(1);auto output_length=builder_.fresh();builder_.emit(Binary{output_length,"+",source_length,one,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64)});
            auto output=builder_.fresh();builder_.emit(ArrayAlloc{output,output_length,result_type});
            const auto zero=builder_.const_int(0);copy_into(source,source_length,output,zero,"append.copy");
            auto appended=lowerer_.lower_into(*n.args[0].value,element_type);
            builder_.emit(ArraySet{output,source_length,appended,element_type});
            if(source_owned) builder_.emit(Release{source,receiver_type});
            return output;
        }
        const bool other_owned=lifetime_.expression_owns_result(*n.args[0].value);
        auto other=lowerer_.lower(*n.args[0].value),other_length=builder_.fresh();builder_.emit(ArrayLength{other_length,other});
        auto output_length=builder_.fresh();builder_.emit(Binary{output_length,"+",source_length,other_length,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64)});
        auto output=builder_.fresh();builder_.emit(ArrayAlloc{output,output_length,result_type});
        const auto zero=builder_.const_int(0);copy_into(source,source_length,output,zero,"concat.left");copy_into(other,other_length,output,source_length,"concat.right");
        if(source_owned) builder_.emit(Release{source,receiver_type});
        if(other_owned) builder_.emit(Release{other,type_of(checked_,*n.args[0].value)});
        return output;
    }
    return std::nullopt;
}

ValueId AggregateLowering::lower_array_allocation(const Expr& e, const CallExpr& n) {
    auto count=lowerer_.lower_int64(*n.args[0].value);
    const auto t=checked_.raw_types.at(&e);
    const bool has_fill=n.args.size()!=1;
    auto out=builder_.fresh();
    builder_.emit(ArrayAlloc{out,count,t,has_fill});
    if(!has_fill) return out;

    const auto& fill_expression=*n.args[1].value;
    bool zero_fill=false;
    if(const auto* value=std::get_if<IntegerExpr>(&fill_expression.data))
        // A zeroed word is the inline integer 0.
        zero_fill=value->value==0 && (is_fixed_integer(*t.first) || is_bare_integer(*t.first));
    else if(const auto* value=std::get_if<RealLiteralExpr>(&fill_expression.data))
        zero_fill=value->value==0.0 && !std::signbit(value->value) && is_fixed_real(*t.first);
    else if(const auto* value=std::get_if<BoolExpr>(&fill_expression.data))
        zero_fill=!value->value && t.first->kind==TypeKind::Bool;
    if(zero_fill) return out;

    auto fill=lowerer_.lower(fill_expression);
    auto idxname=builder_.hidden("fill.index");
    scope_.local_type(idxname)=Type::simple(TypeKind::Int64);
    auto zero=builder_.const_int(0);
    builder_.emit(StoreLocal{idxname,zero,scope_.local_type(idxname)});
    auto cond=builder_.label("fill.cond"),body=builder_.label("fill.body"),done=builder_.label("fill.end");
    builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(cond));
    auto idx=builder_.fresh(),cmp=builder_.fresh();
    builder_.emit(LoadLocal{idx,idxname,scope_.local_type(idxname)});
    builder_.emit(
        Binary{cmp,"<",idx,count,scope_.local_type(idxname),Type::simple(TypeKind::Bool)});
    builder_.emit(Branch{cmp,body,done});
    builder_.enter(builder_.add_block(body));
    auto value=conversions_.convert(fill,*t.first,*t.first,true);
    builder_.emit(ArraySet{
        out,idx,value,*t.first,0,0,true,true});
    auto one=builder_.const_int(1),next=builder_.fresh();
    builder_.emit(
        Binary{next,"+",idx,one,scope_.local_type(idxname),scope_.local_type(idxname)});
    builder_.emit(StoreLocal{idxname,next,scope_.local_type(idxname)});
    builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(done));
    lifetime_.release_temporary(*n.args[1].value,fill);
    return out;
}

ValueId AggregateLowering::lower_len(const Expr& e, const CallExpr& n) {
    auto value=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    const auto kind=type_of(checked_,*n.args[0].value).kind;
    if(kind==TypeKind::Bin)builder_.emit(BinLength{out,value});
    else if(kind==TypeKind::String)builder_.emit(StringLength{out,value});
    else builder_.emit(ArrayLength{out,value});
    lifetime_.release_temporary(*n.args[0].value,value);
    // A length is within [0, 2^62): its word is inline.
    return lowerer_.bare_from_int64(out,type_of(checked_,e),true);
}
} // namespace quidra::lowering
