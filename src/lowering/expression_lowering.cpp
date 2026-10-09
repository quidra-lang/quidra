// Expression lowering, the expression recursion roots:
//   lower_at_raw_type  an expression at its raw type (under the expression
//                      nesting budget), with its run-time initialization
//                      check before it and the initialization flags it sets
//                      after it (initialization_flags.hpp); lower_kind
//                      lowers an enum construction, then one function per
//                      kind of expression, tried in a fixed order (binary
//                      operators are in operator_lowering.cpp, calls and
//                      method calls in call_lowering.cpp)
//   lower              converted to its checked type (or the value
//                      precompute lowered earlier)
//   lower_into         converted to a destination type, consuming fail-fast
//                      values
//   lower_address      the address of a writable place

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/language.hpp"
#include <stdexcept>

namespace quidra::lowering {

ValueId ExpressionLowering::lower_into(const Expr& expression, const Type& target) {
    auto value = lower_at_raw_type(expression);
    const auto source = checked_.raw_types.at(&expression);
    const bool owned = lifetime_.expression_owns_result(expression);
    if (checked_.fail_fast_expressions.contains(&expression)) {
        return conversions_.consume_fail_fast(value, source, target, owned, expression.span);
    }
    if (owned && source.kind == TypeKind::Union && source != target) {
        auto converted = conversions_.convert(value, source, target, true);
        builder_.emit(Release{value, source});
        return converted;
    }
    auto converted = conversions_.convert(value, source, target, !owned);
    if (owned && array_shape_conversion(source, target)) {
        builder_.emit(Release{value, source});
    }
    return converted;
}

ValueId ExpressionLowering::lower_int64(const Expr& e) {
    const auto source = type_of(checked_, e);
    const auto target = Type::simple(TypeKind::Int64);
    // A `nat(n)` argument is consumed by contextual fail-fast: its success
    // value is owned here.
    const bool fail_fast = checked_.fail_fast_expressions.contains(&e);
    auto value = fail_fast ? lower_into(e, source) : lower(e);
    if (source == target) return value;
    auto out = builder_.fresh();
    builder_.emit(NumericConvert{
        out, value, source, target, false,
        static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column),
        facts_.integer_ranges.conversion_inline_proven(e, source, target)});
    if (fail_fast) builder_.emit(Release{value, source});
    else lifetime_.release_temporary(e, value);
    return out;
}

ValueId ExpressionLowering::bare_from_int64(
    ValueId value, const Type& target, bool proven_inline) {
    auto out = builder_.fresh();
    builder_.emit(NumericConvert{
        out, value, Type::simple(TypeKind::Int64), target, false, 0, 0, proven_inline});
    return out;
}

ValueId ExpressionLowering::lower(const Expr& e) {
    if(const auto found=precomputed_.find(&e);found!=precomputed_.end()){
        const auto value=found->second;
        precomputed_.erase(found);
        return value;
    }
    auto v=lower_at_raw_type(e);
    const auto from=checked_.raw_types.at(&e);
    const auto to=type_of(checked_,e);
    auto converted=conversions_.convert(v,from,to);
    if(lifetime_.expression_owns_result(e)&&array_shape_conversion(from,to))
        builder_.emit(Release{v,from});
    return converted;
}

void ExpressionLowering::precompute(const Expr& e) {
    const auto value=lower(e);
    precomputed_[&e]=value;
}

ValueId ExpressionLowering::lower_address(const Expr& e, bool may_write) {
    initialization_flags_.check(e);
    if (const auto* n=std::get_if<NameExpr>(&e.data)) {
        if (const auto it=checked_.field_accesses.find(&e);it!=checked_.field_accesses.end()) {
            auto object=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
            initialization_flags_.check_field(e,object);
            builder_.emit(AddressField{out,object,it->second.index});
            return out;
        }
        auto out=builder_.fresh();
        if (scope_.is_source_reference(n->name)) {
            builder_.emit(ReferenceAddress{out,scope_.source_reference(n->name)});
        } else {
            if (may_write) {
                facts_.array_bounds.invalidate_length_relation(n->name);
                if (scope_.source_local_type(n->name).kind == TypeKind::Array) {
                    facts_.array_initialization.forget_full(n->name);
                }
            }
            builder_.emit(AddressLocal{out,scope_.source_local(n->name)});
        }
        return out;
    }
    if (const auto* n=std::get_if<MemberExpr>(&e.data)) {
        auto object=lower(*n->base),out=builder_.fresh();
        const auto& info=checked_.field_accesses.at(&e);
        initialization_flags_.check_field(e,object);
        builder_.emit(AddressField{out,object,info.index});
        return out;
    }
    if (const auto* n=std::get_if<IndexExpr>(&e.data)) {
        const auto array_type=type_of(checked_,*n->base);
        if(array_type.kind==TypeKind::Tensor){
            throw std::logic_error("tensor elements do not expose raw addresses");
        }
        auto array=lower(*n->base),index=lower_int64(*n->items.front().index),out=builder_.fresh();
        const auto element_type=array_type.kind==TypeKind::Bin
            ? Type::simple(TypeKind::Nat8)
            : *array_type.first;
        // An element index failure reports the index operand.
        const auto& at=n->items.front().index->span.start;
        builder_.emit(AddressElement{
            out,array,index,array_type,element_type,array_type.kind==TypeKind::Bin,
            static_cast<std::uint32_t>(at.line),
            static_cast<std::uint32_t>(at.column)});
        return out;
    }
    throw std::runtime_error("invalid address target");
}

ValueId ExpressionLowering::lower_at_raw_type(const Expr& e) {
    const auto guard = nesting_depth_.expression(e.span);
    initialization_flags_.check(e);
    const auto value = lower_kind(e);
    initialization_flags_.after(e);
    return value;
}

ValueId ExpressionLowering::lower_kind(const Expr& e) {
    if (const auto construction = checked_.enum_constructions.find(&e);
        construction != checked_.enum_constructions.end())
        return lower_enum_construction(e, construction->second);
    if (const auto* n=std::get_if<IntegerExpr>(&e.data)) return lower_literal(e,*n);
    if (const auto* n=std::get_if<RealLiteralExpr>(&e.data)) return lower_literal(e,*n);
    if (const auto* n=std::get_if<BoolExpr>(&e.data)) return lower_literal(*n);
    if (const auto* n=std::get_if<StringExpr>(&e.data)) return lower_literal(*n);
    if (std::holds_alternative<VoidExpr>(e.data) || std::holds_alternative<NoneExpr>(e.data)) return 0;
    if (const auto* n=std::get_if<StringTemplateExpr>(&e.data)) return lower_string_template(*n);
    if (const auto* n=std::get_if<NameExpr>(&e.data)) return lower_name(e,*n);
    if (const auto* n=std::get_if<MemberExpr>(&e.data)) return lower_member(e,*n);
    if (const auto* n=std::get_if<ArrayExpr>(&e.data)) return lower_array_literal(e,*n);
    if (const auto* n=std::get_if<IndexExpr>(&e.data)) return lower_index(e,*n);
    if (const auto* n=std::get_if<UnaryExpr>(&e.data)) return lower_unary(e,*n);
    if (const auto* n=std::get_if<BinaryExpr>(&e.data)) return operators_.lower_binary(e,*n);
    if (const auto* n=std::get_if<TryExpr>(&e.data)) return lower_try(e,*n);
    if (const auto* n=std::get_if<IfExpr>(&e.data)) return lower_if_expression(e,*n);
    if (const auto* n=std::get_if<MethodCallExpr>(&e.data)) return calls_.lower_method_call(e,*n);
    return calls_.lower_call(e,std::get<CallExpr>(e.data));
}

ValueId ExpressionLowering::lower_enum_construction(
    const Expr& e, const EnumConstructionInfo& construction) {
    ValueId payload = 0;
    if (const auto* call = std::get_if<MethodCallExpr>(&e.data);
        call && !call->args.empty())
        payload = lower_into(*call->args.front().value, construction.payload_type);
    auto out = builder_.fresh();
    builder_.emit(VariantMake{out, construction.tag, payload,
        construction.type, construction.payload_type});
    return out;
}

ValueId ExpressionLowering::lower_literal(const Expr& e, const IntegerExpr& n) {
    auto out=builder_.fresh();
    const auto type=checked_.raw_types.at(&e);
    const auto spelling=n.spelling.empty()?std::to_string(n.value):n.spelling;
    if(is_bare_integer(type)||type.kind==TypeKind::Real) {
        builder_.emit(ConstantExact{out,spelling,type});
    } else if(is_fixed_real(type)) {
        builder_.emit(ConstantFloat{
            out,static_cast<double>(n.value),type,spelling});
    } else {
        builder_.emit(ConstantInt{out,spelling,type});
    }
    return out;
}

ValueId ExpressionLowering::lower_literal(const Expr& e, const RealLiteralExpr& n) {
    auto out=builder_.fresh(); const auto type=checked_.raw_types.at(&e);
    if(type.kind==TypeKind::Real)
        builder_.emit(ConstantExact{
            out,n.spelling.empty()?std::to_string(n.value):n.spelling,type});
    else
        builder_.emit(ConstantFloat{out,n.value,type,n.spelling});
    return out;
}

ValueId ExpressionLowering::lower_literal(const BoolExpr& n) {
    auto out=builder_.fresh();
    builder_.emit(ConstantBool{out,n.value});
    return out;
}

ValueId ExpressionLowering::lower_literal(const StringExpr& n) {
    auto out=builder_.fresh();
    builder_.emit(ConstantString{out,n.value});
    return out;
}

ValueId ExpressionLowering::lower_string_template(const StringTemplateExpr& n) {
    auto current=builder_.fresh();
    builder_.emit(ConstantString{current,n.literals.front()});
    bool current_owned=false;
    for(std::size_t i=0;i<n.expressions.size();++i){
        auto part=lower(*n.expressions[i]);
        const auto pt=type_of(checked_,*n.expressions[i]);
        bool part_owned=lifetime_.expression_owns_result(*n.expressions[i]);
        if(n.formats[i].active()){
            auto converted=builder_.fresh();
            const auto& format=n.formats[i];
            builder_.emit(FormatNumber{converted,part,pt,format.integer_width,
                format.fractional_digits,format.significant_digits,format.zero});
            part=converted;
            part_owned=true;
        }else if(pt.kind!=TypeKind::String && pt.kind!=TypeKind::Error){
            auto converted=builder_.fresh();
            builder_.emit(ToString{converted,part,pt});
            part=converted;
            part_owned=true;
        }
        auto joined=builder_.fresh();
        builder_.emit(Binary{
            joined,"+",current,part,Type::simple(TypeKind::String),
            Type::simple(TypeKind::String)});
        if(current_owned) builder_.emit(
            Release{current,Type::simple(TypeKind::String)});
        if(part_owned) builder_.emit(
            Release{part,Type::simple(TypeKind::String)});
        current=joined;
        current_owned=true;
        if(!n.literals[i+1].empty()){
            auto lit=builder_.fresh();
            builder_.emit(ConstantString{lit,n.literals[i+1]});
            joined=builder_.fresh();
            builder_.emit(Binary{
                joined,"+",current,lit,Type::simple(TypeKind::String),
                Type::simple(TypeKind::String)});
            builder_.emit(
                Release{current,Type::simple(TypeKind::String)});
            current=joined;
        }
    }
    return current;
}

ValueId ExpressionLowering::lower_name(const Expr& e, const NameExpr& n) {
    if(is_builtin_text_constant(n.name)){auto out=builder_.fresh();builder_.emit(ConstantString{out,std::string(builtin_text_constant(n.name))});return out;}
    if(const auto it=checked_.field_accesses.find(&e);it!=checked_.field_accesses.end()){
        auto object=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
        initialization_flags_.check_field(e,object);
        builder_.emit(FieldGet{out,object,it->second.index,it->second.type});return out;
    }
    if (const auto function = checked_.function_references.find(&e);
        function != checked_.function_references.end()) {
        auto out=builder_.fresh();
        builder_.emit(FunctionRef{out,function->second,checked_.raw_types.at(&e)});
        return out;
    }
    auto out=builder_.fresh(); const auto raw=checked_.raw_types.at(&e);
    if(scope_.is_source_reference(n.name)){
        const auto& ir_name=scope_.source_reference(n.name); const auto t=scope_.reference_type_at(ir_name);
        builder_.emit(LoadReference{out,ir_name,t});
        if(t.kind==TypeKind::Union&&raw.kind!=TypeKind::Union){auto pv=builder_.fresh();builder_.emit(VariantPayload{pv,out,raw});return pv;}
        return out;
    }
    const auto& ir_name=scope_.source_local(n.name); const auto t=scope_.local_type_at(ir_name); builder_.emit(LoadLocal{out,ir_name,t}); if(t.kind==TypeKind::Union&&raw.kind!=TypeKind::Union){auto pv=builder_.fresh();builder_.emit(VariantPayload{pv,out,raw});return pv;}return out;
}

ValueId ExpressionLowering::lower_member(const Expr& e, const MemberExpr& n) {
    if (checked_.tensor_grad_accesses.contains(&e)) return autograd_.lower_tensor_grad(e, n);
    const bool base_owned=lifetime_.expression_owns_result(*n.base);
    auto object=lower(*n.base),out=builder_.fresh();
    const auto& info=checked_.field_accesses.at(&e);
    initialization_flags_.check_field(e,object);
    builder_.emit(FieldGet{out,object,info.index,info.type});
    if(base_owned){
        if(requires_lifetime_management(info.type)) out=lifetime_.copy_value(out,info.type);
        builder_.emit(Release{object,type_of(checked_,*n.base)});
    }
    return out;
}

ValueId ExpressionLowering::lower_array_literal(const Expr& e, const ArrayExpr& n) {
    std::vector<ValueId> values; values.reserve(n.elements.size());
    for(const auto& x:n.elements) values.push_back(lower_into(*x,type_of(checked_,*x)));
    auto out=builder_.fresh(); builder_.emit(ArrayMake{out,std::move(values),checked_.raw_types.at(&e)}); return out;
}

ValueId ExpressionLowering::lower_index(const Expr& e, const IndexExpr& n) {
    const bool base_owned=lifetime_.expression_owns_result(*n.base);
    const auto base_type=type_of(checked_,*n.base);
    const bool standard_collection_array =
        base_type.kind == TypeKind::Array &&
        enclosing_class_.is_standard_collection() &&
        checked_.field_accesses.contains(n.base.get());
    const bool initialization_proven =
        base_type.kind == TypeKind::Array &&
        (facts_.array_initialization.fully_initialized(*n.base) ||
         standard_collection_array);
    auto a=lower(*n.base), out=builder_.fresh();
    if(base_type.kind==TypeKind::Tensor){
        std::vector<TensorIndexPart> items;
        items.reserve(n.items.size());
        for(const auto& item:n.items){
            TensorIndexPart lowered;
            lowered.slice=item.slice;
            if(item.index) lowered.index=lower_int64(*item.index);
            if(item.start) lowered.start=lower_int64(*item.start);
            if(item.stop) lowered.stop=lower_int64(*item.stop);
            if(item.step) lowered.step=lower_int64(*item.step);
            items.push_back(std::move(lowered));
        }
        builder_.emit(TensorIndex{
            out,a,std::move(items),base_type,
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)});
    }else{
        if(base_type.kind==TypeKind::Bin) {
            const auto& item=n.items.front();
            if(item.slice){
                auto start=item.start?lower_int64(*item.start):builder_.const_int(0);
                ValueId stop;
                if(item.stop) stop=lower_int64(*item.stop);
                else { stop=builder_.fresh(); builder_.emit(BinLength{stop,a}); }
                // A slice reports the indexing expression.
                builder_.emit(BinSlice{out,a,start,stop,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
            }else{
                // An element index failure reports the index operand.
                auto i=lower_int64(*item.index);
                builder_.emit(BinGet{
                    out,a,i,static_cast<std::uint32_t>(item.index->span.start.line),
                    static_cast<std::uint32_t>(item.index->span.start.column),
                    checked_.bounds_proven.contains(item.index.get())});
            }
        } else if(base_type.kind==TypeKind::String) {
            const auto& index=*n.items.front().index;
            auto i=lower_int64(index);
            builder_.emit(StringIndex{
                out,a,i,static_cast<std::uint32_t>(index.span.start.line),
                static_cast<std::uint32_t>(index.span.start.column)});
        } else {
            auto i=lower_int64(*n.items.front().index);
            const auto initialization_guard = initialization_proven
                ? std::optional<ValueId>{}
                : facts_.array_initialization.reference_guard(*n.base);
            const bool bounds_proven =
                checked_.bounds_proven.contains(
                    n.items.front().index.get()) ||
                standard_collection_array ||
                facts_.array_bounds.proven_by_range_loop(
                    *n.base, *n.items.front().index);
            const auto bounds_guard = bounds_proven
                ? std::optional<ValueId>{}
                : facts_.array_bounds.reference_guard(
                    *n.base, *n.items.front().index);
            builder_.emit(ArrayGet{
                out,a,i,checked_.raw_types.at(&e),
                static_cast<std::uint32_t>(n.items.front().index->span.start.line),
                static_cast<std::uint32_t>(n.items.front().index->span.start.column),
                initialization_proven, bounds_proven,
                initialization_guard, bounds_guard});
            if(base_owned && requires_lifetime_management(checked_.raw_types.at(&e)))
                out=lifetime_.copy_value(out,checked_.raw_types.at(&e));
        }
    }
    if(base_owned) builder_.emit(Release{a,base_type});
    return out;
}

ValueId ExpressionLowering::lower_unary(const Expr& e, const UnaryExpr& n) {
    if(n.op=="&") return lower_address(*n.operand, false);
    if(n.op=="-"){
        if(const auto* literal=std::get_if<IntegerExpr>(&n.operand->data);
           literal && is_fixed_integer(type_of(checked_, e))){
            auto out=builder_.fresh();
            builder_.emit(ConstantInt{
                out,"-"+std::to_string(literal->value),type_of(checked_,e)});
            return out;
        }
        // A negated bare integer literal is a constant of its own.
        if(const auto* literal=std::get_if<IntegerExpr>(&n.operand->data);
           literal && type_of(checked_, e).kind==TypeKind::Int){
            auto out=builder_.fresh();
            const auto digits=literal->spelling.empty()?std::to_string(literal->value):literal->spelling;
            builder_.emit(ConstantExact{out,"-"+digits,type_of(checked_,e)});
            return out;
        }
    }
    auto v=lower(*n.operand), out=builder_.fresh();
    builder_.emit(Unary{
        out,n.op,v,type_of(checked_,e),static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column),
        facts_.integer_ranges.inline_proven(e)});
    // Tensor, bigint and bigreal negation return a new value and
    // leave the operand alone; release an owned temporary operand
    // as the binary operators do.
    lifetime_.release_temporary(*n.operand,v);
    return out;
}

ValueId ExpressionLowering::lower_try(const Expr& e, const TryExpr& n) {
    const bool container_owned=lifetime_.expression_owns_result(*n.value);
    auto container=lower(*n.value),tag=builder_.fresh();
    builder_.emit(VariantTag{tag,container});
    auto src=type_of(checked_,*n.value),result=checked_.raw_types.at(&e);
    auto error_type=Type::simple(TypeKind::Error);
    auto errtag=builder_.const_int(case_index(src,error_type)),cmp=builder_.fresh();
    builder_.emit(Binary{
        cmp,"==",tag,errtag,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
    auto bad=builder_.label("try.error"),ok=builder_.label("try.ok");
    builder_.emit(Branch{cmp,bad,ok});
    builder_.enter(builder_.add_block(bad));
    auto payload=builder_.fresh();
    builder_.emit(VariantPayload{payload,container,error_type});
    auto propagated=conversions_.convert(payload,error_type,builder_.function()->result,container_owned);
    if(container_owned) builder_.emit(Release{container,src});
    lifetime_.release_loop_sources();
    builder_.emit(Return{propagated,builder_.function()->result});
    builder_.enter(builder_.add_block(ok));
    if(result.kind!=TypeKind::Union){
        auto out=builder_.fresh();
        builder_.emit(VariantPayload{out,container,result});
        if(container_owned){
            if(requires_lifetime_management(result)) out=lifetime_.copy_value(out,result);
            builder_.emit(Release{container,src});
        }
        return out;
    }
    const auto done=builder_.label("try.end");
    const auto result_name=builder_.hidden("try.result");
    scope_.local_type(result_name)=result;
    for(auto& ct:result.cases){
        auto yes=builder_.label("try.case"),next=builder_.label("try.next");
        auto num=builder_.const_int(case_index(src,ct)),test=builder_.fresh();
        builder_.emit(Binary{
            test,"==",tag,num,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
        builder_.emit(Branch{test,yes,next});
        builder_.enter(builder_.add_block(yes));
        auto pv=builder_.fresh();
        builder_.emit(VariantPayload{pv,container,ct});
        auto cv=conversions_.convert(pv,ct,result,container_owned);
        if(container_owned) builder_.emit(Release{container,src});
        builder_.emit(StoreLocal{result_name,cv,result,true});
        builder_.emit(Jump{done});
        builder_.enter(builder_.add_block(next));
    }
    builder_.enter(builder_.add_block(done));
    auto out=builder_.fresh();
    builder_.emit(LoadLocal{out,result_name,result});
    return out;
}

// An if-expression: each condition in turn branches to its value or to the
// next condition. Every value is converted to the expression's type as an
// owned value (a borrowed read is copied, as for an initializer) and stored
// in a hidden result local, so the result has one ownership whichever branch
// ran. A branch that does not continue stores nothing.
ValueId ExpressionLowering::lower_if_expression(const Expr& e, const IfExpr& n) {
    const auto result=checked_.raw_types.at(&e);
    const bool has_value=result.kind!=TypeKind::Void && result.kind!=TypeKind::None &&
                         result.kind!=TypeKind::Never;
    std::string result_name;
    if(has_value){
        result_name=builder_.hidden("if.result");
        scope_.local_type(result_name)=result;
    }
    const auto done=builder_.label("if.end");
    const auto lower_branch=[&](const Expr& value){
        auto v=lower_into(value,result);
        if(current_block_ended(builder_)) return;
        if(has_value) builder_.emit(StoreLocal{result_name,v,result,true});
        builder_.emit(Jump{done});
    };
    for(std::size_t i=0;i<n.conditions.size();++i){
        auto condition=lower(*n.conditions[i]);
        const auto yes=builder_.label("if.then");
        const auto next=builder_.label("if.next");
        builder_.emit(Branch{condition,yes,next});
        builder_.enter(builder_.add_block(yes));
        lower_branch(*n.values[i]);
        builder_.enter(builder_.add_block(next));
    }
    lower_branch(*n.otherwise);
    builder_.enter(builder_.add_block(done));
    if(!has_value) return 0;
    auto out=builder_.fresh();
    builder_.emit(LoadLocal{out,result_name,result});
    return out;
}

} // namespace quidra::lowering
