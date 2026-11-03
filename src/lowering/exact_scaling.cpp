#include "for_loop_constants.hpp"
#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <charconv>
#include <functional>
#include <optional>
#include <vector>

namespace quidra::lowering {
namespace {
using Rational=for_constants::Rational;
using Integer=for_constants::Integer;
struct ScaledOperation {std::string op;Rational value;bool constant_left{};};
struct ScaledRecipe {std::string name;std::vector<ScaledOperation> operations;};
std::optional<Rational> rational_literal(const Expr& e,unsigned depth=0) {
    if(depth>128) return std::nullopt;
    if(const auto value=for_constants::integer_literal(e)) return Rational{*value,Integer(1)};
    if(const auto* value=std::get_if<RealLiteralExpr>(&e.data)) {
        ieee_decimal::Decimal decimal;
        if(!ieee_decimal::parse(value->spelling,decimal)) return std::nullopt;
        const auto power=decimal.exponent-static_cast<long long>(decimal.fraction_digits.size());
        if(power>4096||power< -4096) return std::nullopt;
        std::string digits(decimal.integer_digits);digits+=decimal.fraction_digits;
        Rational result{Integer(digits),Integer(1)};
        for(long long i=0;i<(power<0?-power:power);++i)
            if(power<0) result.denominator=result.denominator*Integer(10);
            else result.numerator=result.numerator*Integer(10);
        if(decimal.negative) result.numerator=-result.numerator;
        return result;
    }
    if(const auto* unary=std::get_if<UnaryExpr>(&e.data);unary&&unary->op=="-") {
        if(auto result=rational_literal(*unary->operand,depth+1)) {
            result->numerator=-result->numerator;return result;
        }
    }
    if(const auto* binary=std::get_if<BinaryExpr>(&e.data)) {
        const auto a=rational_literal(*binary->left,depth+1),b=rational_literal(*binary->right,depth+1);
        if(!a||!b) return std::nullopt;
        Rational out;
        if(binary->op=="+"||binary->op=="-") {
            out={a->numerator*b->denominator+
                (binary->op=="+"?b->numerator:-b->numerator)*a->denominator,
                a->denominator*b->denominator};
        } else if(binary->op=="*") out={a->numerator*b->numerator,a->denominator*b->denominator};
        else if(binary->op=="/"&&!b->numerator.zero()) {
            out={a->numerator*b->denominator,a->denominator*b->numerator};
            if(for_constants::negative(out.denominator)) {out.numerator=-out.numerator;out.denominator=-out.denominator;}
        } else return std::nullopt;
        return out;
    }
    return std::nullopt;
}
std::optional<ScaledRecipe> scaled_recipe(const Expr& e,const LocalScope& scope,unsigned depth=0) {
    if(depth>128) return std::nullopt;
    if(const auto* name=std::get_if<NameExpr>(&e.data);name&&!name->this_qualifier&&scope.scaled_real(name->name))
        return ScaledRecipe{name->name,{}};
    const auto* binary=std::get_if<BinaryExpr>(&e.data);
    if(!binary||(binary->op!="+"&&binary->op!="-"&&binary->op!="*")) return std::nullopt;
    if(auto recipe=scaled_recipe(*binary->left,scope,depth+1))
        if(auto constant=rational_literal(*binary->right)) {
            recipe->operations.push_back({binary->op,*constant,false});return recipe;
        }
    if(auto recipe=scaled_recipe(*binary->right,scope,depth+1))
        if(auto constant=rational_literal(*binary->left)) {
            recipe->operations.push_back({binary->op,*constant,true});return recipe;
        }
    return std::nullopt;
}
std::optional<long long> signed_word(const Integer& value) {
    const auto& text=value.text();long long result=0;
    const auto conversion=std::from_chars(text.data(),text.data()+text.size(),result);
    if(conversion.ec!=std::errc{}||conversion.ptr!=text.data()+text.size()) return std::nullopt;
    return result;
}
}

std::optional<ValueId> ExpressionLowering::try_scaled_expression(const Expr& e,const Type* cast_target) {
    const Expr* input=&e;
    std::optional<Rational> comparison;
    std::string op;
    if(cast_target) {
        const auto* call=std::get_if<CallExpr>(&e.data);
        if(!call||call->args.size()!=1||!is_fixed_real(*cast_target)) return std::nullopt;
        input=call->args[0].value.get();
    } else if(const auto* binary=std::get_if<BinaryExpr>(&e.data);
        binary&&(binary->op=="=="||binary->op=="!="||binary->op=="<"||binary->op=="<="||binary->op==">"||binary->op==">=")) {
        if(scaled_recipe(*binary->left,scope_)) {
            input=binary->left.get();comparison=rational_literal(*binary->right);op=binary->op;
        } else if(scaled_recipe(*binary->right,scope_)) {
            input=binary->right.get();comparison=rational_literal(*binary->left);
            op=binary->op=="<"?">":binary->op=="<="?">=":binary->op==">"?"<":binary->op==">="?"<=":binary->op;
        } else return std::nullopt;
        if(!comparison) return std::nullopt;
    }
    const auto recipe=scaled_recipe(*input,scope_);
    if(!recipe||(!cast_target&&!comparison&&recipe->operations.empty())) return std::nullopt;
    if(!cast_target&&!comparison&&checked_.raw_types.at(&e).kind!=TypeKind::Real) return std::nullopt;
    const auto p=comparison?signed_word(comparison->numerator):std::optional<long long>{0};
    const auto q=comparison?signed_word(comparison->denominator):std::optional<long long>{1};
    if(!p||!q) return std::nullopt;
    const auto binding=*scope_.scaled_real(recipe->name);
    const auto integer=Type::simple(TypeKind::Int),boolean=Type::simple(TypeKind::Bool);
    const auto result_type=cast_target?checked_.raw_types.at(&e):comparison?boolean:Type::simple(TypeKind::Real);
    const auto result_name=builder_.hidden("scaled.result");scope_.local_type(result_name)=result_type;
    const auto scaled=builder_.label("scaled.rational"),general=builder_.label("scaled.general"),done=builder_.label("scaled.end");
    const auto load=[&](const std::string& name,const Type& type) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,name,type});return value;
    };
    const auto constant=[&](const Integer& value) {
        auto out=builder_.fresh();builder_.emit(ConstantExact{out,value.text(),integer});return out;
    };
    auto is_general=builder_.fresh();
    builder_.emit(Binary{is_general,"!=",load(binding.general,integer),constant(Integer(0)),integer,boolean});
    builder_.emit(Branch{is_general,general,scaled});
    builder_.enter(builder_.add_block(general));
    materialize_scaled_real(recipe->name);
    scope_.erase_scaled_real(recipe->name);
    auto ordinary=lower_at_raw_type(e);
    scope_.bind_scaled_real(recipe->name,binding);
    builder_.emit(StoreLocal{result_name,ordinary,result_type});builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(scaled));
    std::vector<std::string> owned;
    const auto keep=[&](ValueId value) {
        auto name=builder_.hidden("scaled.integer");scope_.local_type(name)=integer;
        builder_.emit(StoreLocal{name,value,integer});owned.push_back(name);return load(name,integer);
    };
    const auto binary=[&](const std::string& operation,ValueId left,ValueId right) {
        auto out=builder_.fresh();builder_.emit(Binary{out,operation,left,right,integer,integer});return keep(out);
    };
    const auto numerator_name=builder_.hidden("scaled.numerator");scope_.local_type(numerator_name)=integer;owned.push_back(numerator_name);
    auto machine=builder_.fresh();builder_.emit(Binary{machine,"!=",load(binding.machine_flag,integer),constant(Integer(0)),integer,boolean});
    const auto small=builder_.label("scaled.machine"),wide=builder_.label("scaled.wide"),ready=builder_.label("scaled.ready");
    builder_.emit(Branch{machine,small,wide});builder_.enter(builder_.add_block(small));
    auto numerator=bare_from_int64(load(binding.machine_numerator,Type::simple(TypeKind::Int64)),integer,false);
    builder_.emit(StoreLocal{numerator_name,numerator,integer});builder_.emit(Jump{ready});
    builder_.enter(builder_.add_block(wide));
    builder_.emit(StoreLocal{numerator_name,lifetime_.copy_value(load(binding.numerator,integer),integer),integer});
    builder_.emit(Jump{ready});builder_.enter(builder_.add_block(ready));
    numerator=load(numerator_name,integer);auto denominator=load(binding.denominator,integer);
    for(const auto& operation:recipe->operations) {
        auto a=keep(constant(operation.value.numerator)),b=keep(constant(operation.value.denominator));
        if(operation.op=="*") numerator=binary("*",numerator,a);
        else {
            auto left=binary("*",numerator,b),right=binary("*",a,denominator);
            numerator=operation.constant_left?binary(operation.op,right,left):binary(operation.op,left,right);
        }
        denominator=binary("*",denominator,b);
    }
    auto result=builder_.fresh();
    if(cast_target) builder_.emit(ExactScaledConvert{result,numerator,denominator,*cast_target,
        {static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)},result_type});
    else if(comparison) builder_.emit(ExactScaledCompare{result,numerator,denominator,*p,*q,op});
    else builder_.emit(ExactFromScaled{result,numerator,denominator});
    builder_.emit(StoreLocal{result_name,result,result_type});
    for(const auto& name:owned) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,name,integer,true});release_owned(builder_,value,integer);
    }
    builder_.emit(Jump{done});builder_.enter(builder_.add_block(done));
    auto out=builder_.fresh();builder_.emit(LoadLocal{out,result_name,result_type,owns_managed(result_type)});
    return out;
}
} // namespace quidra::lowering
