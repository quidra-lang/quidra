// Conversion lowering: a value of one type to another. Union payloads are
// dispatched on their tag, values are wrapped into unions, numbers are
// converted, arrays are converted element by element when their shape type
// changes, and a fail-fast union is consumed (error: fail at the span;
// value: continue with the payload). Also the conversions spelled as
// methods: int.parse (and every numeric type's parse), bin.parse, and an
// error's .string(); and numeric casts (int(x), float32(xs), bin(x), ...).

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <optional>
#include <stdexcept>

namespace quidra::lowering {

bool array_shape_conversion(const Type& from, const Type& to) {
    return from.kind == TypeKind::Array && to.kind == TypeKind::Array && from != to;
}

ValueId ConversionLowering::consume_fail_fast(ValueId container, const Type& source,
                          const Type& target, bool owned, SourceSpan span) {
    const auto error_type = Type::simple(TypeKind::Error);
    const int error_tag_index = case_index(source, error_type);
    if (source.kind != TypeKind::Union || !source.union_name.empty() ||
        error_tag_index < 0) {
        throw std::logic_error("fail-fast consumption requires an unnamed error union");
    }

    auto tag = builder_.fresh();
    builder_.emit(VariantTag{tag, container});
    auto error_tag = builder_.const_int(error_tag_index);
    auto is_error = builder_.fresh();
    builder_.emit(Binary{
        is_error, "==", tag, error_tag,
        Type::simple(TypeKind::Int64), Type::simple(TypeKind::Bool)});

    const auto failed = builder_.label("fail_fast.error");
    const auto dispatch = builder_.label("fail_fast.value");
    builder_.emit(Branch{is_error, failed, dispatch});

    builder_.enter(builder_.add_block(failed));
    auto problem = builder_.fresh();
    builder_.emit(
        VariantPayload{problem, container, error_type});
    builder_.emit(FailError{
        problem,
        static_cast<std::uint32_t>(span.start.line),
        static_cast<std::uint32_t>(span.start.column)});

    std::vector<Type> remaining;
    remaining.reserve(source.cases.size() - 1);
    for (const auto& current : source.cases) {
        if (current.kind != TypeKind::Error) remaining.push_back(current);
    }
    if (remaining.empty()) {
        throw std::logic_error("fail-fast union has no success alternative");
    }

    builder_.enter(builder_.add_block(dispatch));
    const bool has_value =
        target.kind != TypeKind::Void && target.kind != TypeKind::None;

    if (remaining.size() == 1) {
        auto payload = builder_.fresh();
        builder_.emit(
            VariantPayload{payload, container, remaining.front()});
        auto result = convert(payload, remaining.front(), target, true);
        if (owned) builder_.emit(Release{container, source});
        return has_value ? result : 0;
    }

    const auto done = builder_.label("fail_fast.done");
    std::string result_name;
    if (has_value) {
        result_name = builder_.hidden("fail_fast.result");
        scope_.local_type(result_name) = target;
    }

    const auto lower_case = [&](const Type& current) {
        auto payload = builder_.fresh();
        builder_.emit(
            VariantPayload{payload, container, current});
        auto result = convert(payload, current, target, true);
        if (has_value) {
            builder_.emit(
                StoreLocal{result_name, result, target, true});
        }
        if (owned) builder_.emit(Release{container, source});
        builder_.emit(Jump{done});
    };

    for (std::size_t i = 0; i + 1 < remaining.size(); ++i) {
        const auto yes = builder_.label("fail_fast.case");
        const auto next = builder_.label("fail_fast.next");
        auto current_tag = builder_.const_int(case_index(source, remaining[i]));
        auto matches = builder_.fresh();
        builder_.emit(Binary{
            matches, "==", tag, current_tag,
            Type::simple(TypeKind::Int64), Type::simple(TypeKind::Bool)});
        builder_.emit(Branch{matches, yes, next});
        builder_.enter(builder_.add_block(yes));
        lower_case(remaining[i]);
        builder_.enter(builder_.add_block(next));
    }
    lower_case(remaining.back());

    builder_.enter(builder_.add_block(done));
    if (!has_value) return 0;
    auto out = builder_.fresh();
    builder_.emit(LoadLocal{out, result_name, target});
    return out;
}

ValueId ConversionLowering::convert_array_shape(ValueId source,const Type& from,const Type& to) {
    auto length=builder_.fresh();
    if(from.length>=0){
        builder_.emit(
            ConstantInt{length,std::to_string(from.length),Type::simple(TypeKind::Int64)});
    }else{
        builder_.emit(ArrayLength{length,source});
    }

    auto output=builder_.fresh();
    builder_.emit(ArrayAlloc{output,length,to});

    const auto index_name=builder_.hidden("array.convert.index");
    scope_.local_type(index_name)=Type::simple(TypeKind::Int64);
    const auto zero=builder_.const_int(0);
    builder_.emit(StoreLocal{index_name,zero,scope_.local_type(index_name)});

    const auto cond=builder_.label("array.convert.cond");
    const auto body=builder_.label("array.convert.body");
    const auto done=builder_.label("array.convert.done");
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(cond));
    auto index=builder_.fresh(),more=builder_.fresh();
    builder_.emit(LoadLocal{index,index_name,scope_.local_type(index_name)});
    builder_.emit(Binary{
        more,"<",index,length,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
    builder_.emit(Branch{more,body,done});

    builder_.enter(builder_.add_block(body));
    auto item=builder_.fresh();
    builder_.emit(ArrayGet{item,source,index,*from.first});
    auto converted=convert(item,*from.first,*to.first,true);
    builder_.emit(ArraySet{output,index,converted,*to.first});

    const auto one=builder_.const_int(1);
    auto next=builder_.fresh();
    builder_.emit(Binary{
        next,"+",index,one,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Int64)});
    builder_.emit(StoreLocal{index_name,next,scope_.local_type(index_name)});
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(done));
    return output;
}

ValueId ConversionLowering::convert(ValueId v,const Type& from,const Type& to,bool copy) {
    if(from.kind==TypeKind::Never)return v;
    if(array_shape_conversion(from,to)) return convert_array_shape(v,from,to);
    if(from.kind==TypeKind::Union && (from!=to||copy)) {
        auto tag=builder_.fresh();
        builder_.emit(VariantTag{tag,v});
        const auto done=builder_.label("union.end");
        const bool has_value=to.kind!=TypeKind::Void && to.kind!=TypeKind::None;
        std::string result_name;
        if(has_value){
            result_name=builder_.hidden("union.convert.result");
            scope_.local_type(result_name)=to;
        }
        for(std::size_t i=0;i<from.cases.size();++i){
            const auto yes=builder_.label("union.case");
            const auto next=builder_.label("union.next");
            auto n=builder_.const_int(i),cmp=builder_.fresh();
            builder_.emit(Binary{
                cmp,"==",tag,n,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
            builder_.emit(Branch{cmp,yes,next});
            builder_.enter(builder_.add_block(yes));
            const auto& ct=from.cases[i];
            auto payload=builder_.fresh();
            builder_.emit(VariantPayload{payload,v,ct});
            auto result=convert(payload,ct,to,copy);
            if(has_value) builder_.emit(StoreLocal{result_name,result,to,true});
            builder_.emit(Jump{done});
            builder_.enter(builder_.add_block(next));
        }
        builder_.enter(builder_.add_block(done));
        if(!has_value) return 0;
        auto out=builder_.fresh();
        builder_.emit(LoadLocal{out,result_name,to});
        return out;
    }
    if(to.kind==TypeKind::Union && from.kind!=TypeKind::Union){if(copy)v=lifetime_.copy_value(v,from);auto out=builder_.fresh();builder_.emit(VariantMake{out,compatible_case_index(to,from),v,to,from});return out;}
    if(from!=to && is_numeric(from) && is_numeric(to)){
        auto out=builder_.fresh();
        builder_.emit(NumericConvert{out,v,from,to,false});
        v=out;
    }
    if(copy)return lifetime_.copy_value(v,to);
    return v;
}

std::optional<ValueId> ConversionLowering::try_parse_method(
    const Expr& e, const MethodCallExpr& n) {
    const auto* receiver_name=std::get_if<NameExpr>(&n.receiver->data);
    if(receiver_name && n.method=="parse"){
        const auto target=builtin_scalar_type(receiver_name->name);
        if(target && is_numeric(*target)){
            auto text=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(ParseNumber{out,text,*target,checked_.raw_types.at(&e)});
            lifetime_.release_temporary(*n.args[0].value,text);
            return out;
        }
        if(target && target->kind==TypeKind::Bin){
            auto text=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(ParseBin{
                out,text,checked_.raw_types.at(&e),
                std::holds_alternative<StringExpr>(n.args[0].value->data)});
            lifetime_.release_temporary(*n.args[0].value,text);
            return out;
        }
    }
    return std::nullopt;
}

std::optional<ValueId> ConversionLowering::try_error_method(
    const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::Error && n.method=="string"){
        return lowerer_.lower(*n.receiver);
    }
    return std::nullopt;
}

ValueId ConversionLowering::lower_numeric_cast(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    auto value=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    const auto source=type_of(checked_,*n.args[0].value);
    const auto scalar_target=builtin_scalar_type(resolution.target);
    const auto result_type=checked_.raw_types.at(&e);
    auto success_type=result_type;
    if(result_type.kind==TypeKind::Union && result_type.union_name.empty() &&
       case_index(result_type,Type::simple(TypeKind::Error))>=0){
        std::vector<Type> success_cases;
        for(const auto& candidate:result_type.cases)
            if(candidate.kind!=TypeKind::Error) success_cases.push_back(candidate);
        if(success_cases.empty())
            throw std::logic_error("numeric cast error union has no success alternative");
        success_type=Type::union_of(std::move(success_cases));
    }
    // A fallible container cast carries its whole converted container
    // on the success side of T | error. Element unions are never formed.
    const bool container_source =
        source.kind==TypeKind::Array || source.kind==TypeKind::Tensor ||
        source.kind==TypeKind::Bin;
    const auto target =
        !container_source && scalar_target && is_numeric(*scalar_target)
            ? *scalar_target
            : success_type;
    if(source.kind==TypeKind::Bin){
        builder_.emit(BinConvert{
            out,value,source,target,
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)});
        lifetime_.release_temporary(*n.args[0].value,value);
    }else if(source.kind==TypeKind::Tensor){
        builder_.emit(TensorCast{
            out,value,source,target,result_type,
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)});
        lifetime_.release_temporary(*n.args[0].value,value);
    }else if(source.kind==TypeKind::Array){
        builder_.emit(ArrayNumericCast{
            out,value,source,target,result_type,
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)});
        lifetime_.release_temporary(*n.args[0].value,value);
    }else{
        const bool recoverable_range =
            result_type.kind == TypeKind::Union &&
            result_type.union_name.empty() &&
            case_index(result_type, Type::simple(TypeKind::Error)) >= 0 &&
            numeric_conversion_policy(source,target)==NumericConversionPolicy::ExplicitRangeCheck;
        if (recoverable_range) {
            builder_.emit(FallibleNumericConvert{
                out,value,source,target,result_type,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
        } else {
            const bool checked_range =
                numeric_conversion_policy(source,target)==NumericConversionPolicy::ExplicitRangeCheck;
            builder_.emit(NumericConvert{
                out,value,source,target,checked_range,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column),
                lowerer_.facts().integer_ranges.conversion_inline_proven(*n.args[0].value,source,target)});
        }
        lifetime_.release_temporary(*n.args[0].value,value);
    }
    return out;
}
} // namespace quidra::lowering
