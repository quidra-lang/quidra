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
#include "lowering/complex_literals.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/abi/tensor_index.hpp"
#include "quidra/language.hpp"
#include <stdexcept>

namespace quidra::lowering {

ValueId ExpressionLowering::lower_into(const Expr& expression, const Type& target) {
    auto value = lower_at_raw_type(expression);
    const auto source = checked_.raw_types.at(&expression);
    const bool owned = lifetime_.expression_owns_result(expression);
    if (checked_.fail_fast_expressions.contains(&expression)) {
        const auto hop = owned ? errors_.hop_call(expression) : std::nullopt;
        return conversions_.consume_fail_fast(value, source, target, owned, expression.span,
                                              hop ? &*hop : nullptr);
    }
    if (owned && source.kind == TypeKind::Union && source != target) {
        auto converted = conversions_.convert(value, source, target, true);
        release_owned(builder_,value, source);
        return converted;
    }
    auto converted = conversions_.convert(value, source, target, !owned);
    if (owned && array_shape_conversion(source, target)) {
        release_owned(builder_,value, source);
    }
    return converted;
}

ValueId ExpressionLowering::lower_int64(const Expr& e) {
    if(const auto captured=captured_.find(&e);captured!=captured_.end()) return captured->second;
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
        {static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)},
        facts_.integer_ranges.conversion_inline_proven(e, source, target)});
    if (fail_fast) release_owned(builder_,value, source);
    else lifetime_.release_temporary(e, value);
    return out;
}

SliceBounds ExpressionLowering::lower_slice_bounds(const Expr& e, const IndexPart& item) {
    SliceBounds bounds;
    if(item.start) bounds.start=lower_int64(*item.start);
    if(item.stop) bounds.end=lower_int64(*item.stop);
    if(item.step) bounds.step=lower_int64(*item.step);
    bounds.start_form=item.start_form;
    bounds.end_form=item.end_form;
    bounds.direction=checked_.slice_directions.at(&e);
    return bounds;
}

// A slice of a string: start:<end is today's string slice; every other form
// is planned over code points. Its result is numbered after its operands.
ValueId ExpressionLowering::lower_string_slice(
    const Expr& e, const IndexExpr& n, ValueId text, bool text_owned, const Type& text_type) {
    const auto& item=n.items.front();
    const auto line=static_cast<std::uint32_t>(e.span.start.line);
    const auto column=static_cast<std::uint32_t>(e.span.start.column);
    ValueId out;
    if(item.start_form==SliceStart::inclusive && item.end_form==SliceEnd::exclusive_ascending &&
       !item.step){
        auto start=lower_int64(*item.start),end=lower_int64(*item.stop);
        out=builder_.fresh();
        builder_.emit(StringSlice{out,text,start,end,{line,column}});
    }else{
        auto bounds=lower_slice_bounds(e,item);
        out=builder_.fresh();
        builder_.emit(StringSliceStepped{out,text,std::move(bounds),{line,column}});
    }
    if(text_owned) release_owned(builder_,text,text_type);
    return out;
}

ValueId ExpressionLowering::bare_from_int64(
    ValueId value, const Type& target, bool proven_inline) {
    auto out = builder_.fresh();
    builder_.emit(NumericConvert{
        out, value, Type::simple(TypeKind::Int64), target, false, {0, 0}, proven_inline});
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
        release_owned(builder_,v,from);
    return converted;
}

void ExpressionLowering::precompute(const Expr& e) {
    const auto value=lower(e);
    precomputed_[&e]=value;
}

ValueId ExpressionLowering::lower_address(const Expr& e, bool may_write) {
    initialization_flags_.check(e);
    if(const auto* node=std::get_if<TupleProjectionExpr>(&e.data)) {
        auto address=builder_.fresh();Type type;
        if(scope_.is_source_reference(node->subject)){const auto& name=scope_.source_reference(node->subject);type=scope_.reference_type_at(name);builder_.emit(ReferenceAddress{address,name});}
        else {const auto& name=scope_.source_local(node->subject);type=scope_.local_type_at(name);builder_.emit(AddressLocal{address,name});}
        for(auto index:node->indices){auto child=builder_.fresh();auto next_type=type.parameters.at(index);builder_.emit(AddressTupleElement{child,address,index,type});type=std::move(next_type);address=child;}return address;
    }
    if (const auto* n=std::get_if<NameExpr>(&e.data)) {
        if (n->self_reference) {
            auto out = builder_.fresh();
            if (enclosing_class_.receiver_reference()) builder_.emit(ReferenceAddress{out, enclosing_class_.receiver_local()});
            else builder_.emit(AddressLocal{out, enclosing_class_.receiver_local()});
            return out;
        }
        materialize_scaled_real(n->name);
        if (const auto it=checked_.field_accesses.find(&e);it!=checked_.field_accesses.end()) {
            auto object=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
            initialization_flags_.check_field(e,object);
            if(checked_.classes.at(it->second.owner).fields.at(it->second.index).reference)
                builder_.emit(FieldReferenceAddress{out,object,it->second.index});
            else builder_.emit(AddressField{out,object,it->second.index});
            return out;
        }
        auto out=builder_.fresh();
        if(const auto* value=scope_.source_value(n->name)) {
            const auto type=type_of(checked_,e);
            const auto slot=scope_.value_address(builder_,*value,type);
            builder_.emit(StoreLocal{slot,*value,type,true});builder_.emit(AddressLocal{out,slot});return out;
        }
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
        if(checked_.classes.at(info.owner).fields.at(info.index).reference) builder_.emit(FieldReferenceAddress{out,object,info.index});
        else builder_.emit(AddressField{out,object,info.index});
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
            {static_cast<std::uint32_t>(at.line),
            static_cast<std::uint32_t>(at.column)}});
        return out;
    }
    throw std::runtime_error("invalid address target");
}

ValueId ExpressionLowering::lower_at_raw_type(const Expr& e) {
    const auto guard = nesting_depth_.expression(e.span);
    initialization_flags_.check(e);
    const auto value = lower_kind(e);
    facts_.after_reference_call(e);
    initialization_flags_.after(e);
    return value;
}

ValueId ExpressionLowering::lower_kind(const Expr& e) {
    if (const auto construction = checked_.enum_constructions.find(&e);
        construction != checked_.enum_constructions.end())
        return lower_enum_construction(e, construction->second);
    // A complex literal is one constant value (complex_literals.hpp).
    if (const auto complex = type_of(checked_, e); is_complex(complex))
        if (const auto parts = complex_literal_parts(e))
            return emit_complex_literal(builder_, *parts, complex);
    if (const auto* n=std::get_if<IntegerExpr>(&e.data)) return lower_literal(e,*n);
    if (const auto* n=std::get_if<RealLiteralExpr>(&e.data)) return lower_literal(e,*n);
    if (const auto* n=std::get_if<LiteralConstantExpr>(&e.data)) return lower_literal_constant(e,*n);
    if (const auto* n=std::get_if<BoolExpr>(&e.data)) return lower_literal(*n);
    if (const auto* n=std::get_if<StringExpr>(&e.data)) return lower_literal(*n);
    if (std::holds_alternative<VoidExpr>(e.data) || std::holds_alternative<NoneExpr>(e.data)) return 0;
    if (const auto* n=std::get_if<StringTemplateExpr>(&e.data)) return lower_string_template(*n);
    if (const auto* n=std::get_if<NameExpr>(&e.data)) return lower_name(e,*n);
    if (const auto* n=std::get_if<MemberExpr>(&e.data)) return lower_member(e,*n);
    if(const auto* node=std::get_if<TupleProjectionExpr>(&e.data)) {
        auto value=builder_.fresh();Type type=checked_.pattern_subject_types.at(node->subject);
        if(const auto* direct=scope_.source_value(node->subject))value=*direct;
        else if(scope_.is_source_reference(node->subject)){const auto& name=scope_.source_reference(node->subject);type=scope_.reference_type_at(name);builder_.emit(LoadReference{value,name,type});}
        else {const auto& name=scope_.source_local(node->subject);type=scope_.local_type_at(name);builder_.emit(LoadLocal{value,name,type});}
        for(std::size_t position=0;position<node->indices.size();++position){auto index=node->indices[position];auto child=builder_.fresh();auto next_type=type.parameters.at(index);type=std::move(next_type);builder_.emit(TupleGet{child,value,index,type,lifetime_.tuple_moves(node->subject) && position+1==node->indices.size() && owns_managed(type)});value=child;}return value;
    }
    if(const auto* n=std::get_if<TupleExpr>(&e.data)) {
        std::vector<ValueId> elements;
        for(const auto& element:n->elements) elements.push_back(lower_into(*element,type_of(checked_,*element)));
        auto out=builder_.fresh();builder_.emit(TupleMake{out,std::move(elements),checked_.raw_types.at(&e)});return out;
    }
    if (const auto* n=std::get_if<ArrayExpr>(&e.data)) return lower_array_literal(e,*n);
    if (const auto* n=std::get_if<ArrayComprehensionExpr>(&e.data)) return lower_array_comprehension(e,*n);
    if (const auto* n=std::get_if<IndexExpr>(&e.data)) return lower_index(e,*n);
    if (const auto* n=std::get_if<UnaryExpr>(&e.data)) return lower_unary(e,*n);
    if (const auto* n=std::get_if<BinaryExpr>(&e.data)) return operators_.lower_binary(e,*n);
    if (const auto* n=std::get_if<ComparisonChainExpr>(&e.data)) return operators_.lower_comparison_chain(e,*n);
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

// A contextual constant: its exact value in a `real` context; in a
// fixed-width one, a lone literal's rounded constant, or else the value its
// slot holds (computed by the entry), or, in a library, the rounding at the
// use.
ValueId ExpressionLowering::lower_literal_constant(const Expr& e, const LiteralConstantExpr& n) {
    const auto type=checked_.raw_types.at(&e);
    if(type.kind==TypeKind::Real)return lower(*n.value);
    const Expr* literal=n.value.get();
    bool negative=false;
    if(const auto* unary=std::get_if<UnaryExpr>(&literal->data);unary&&unary->op=="-"){
        negative=true;
        literal=unary->operand.get();
    }
    std::optional<ValueId> constant;
    if(const auto* real=std::get_if<RealLiteralExpr>(&literal->data)){
        constant=builder_.fresh();
        builder_.emit(ConstantFloat{*constant,real->value,type,real->spelling});
    }else if(const auto* integer=std::get_if<IntegerExpr>(&literal->data)){
        constant=builder_.fresh();
        builder_.emit(ConstantFloat{*constant,static_cast<double>(integer->value),type,
            integer->spelling.empty()?std::to_string(integer->value):integer->spelling});
    }
    if(constant){
        if(!negative)return *constant;
        auto out=builder_.fresh();
        builder_.emit(Unary{out,"-",*constant,type});
        return out;
    }
    auto out=builder_.fresh();
    if(const auto slot=checked_.literal_constant_slots.find(&e);
       slot!=checked_.literal_constant_slots.end()){
        builder_.emit(ExactConstant{out,literal_constant_slot_name(checked_,slot->second),type});
        return out;
    }
    auto value=lower(*n.value);
    builder_.emit(ExactRound{out,value,type,"",
        {static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.value,value);
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
            if(part_owned) release_owned(builder_,part,pt);
            part=converted;
            part_owned=true;
        }else if(pt.kind==TypeKind::Error){
            // An error part is its message, read before the join.
            auto message=builder_.fresh();
            builder_.emit(ErrorMessage{message,part});
            if(part_owned) release_owned(builder_,part,pt);
            part=message;
            part_owned=true;
        }else if(pt.kind!=TypeKind::String){
            auto converted=builder_.fresh();
            if(pt.kind==TypeKind::Tuple || pt.kind==TypeKind::Union)converted=value_text(builder_,part,pt);
            else builder_.emit(ToString{converted,part,pt});
            if(part_owned) release_owned(builder_,part,pt);
            part=converted;
            part_owned=true;
        }
        auto joined=builder_.fresh();
        builder_.emit(Binary{
            joined,"+",current,part,Type::simple(TypeKind::String),
            Type::simple(TypeKind::String)});
        if(current_owned) release_owned(builder_,current,Type::simple(TypeKind::String));
        if(part_owned) release_owned(builder_,part,Type::simple(TypeKind::String));
        current=joined;
        current_owned=true;
        if(!n.literals[i+1].empty()){
            auto lit=builder_.fresh();
            builder_.emit(ConstantString{lit,n.literals[i+1]});
            joined=builder_.fresh();
            builder_.emit(Binary{
                joined,"+",current,lit,Type::simple(TypeKind::String),
                Type::simple(TypeKind::String)});
            release_owned(builder_,current,Type::simple(TypeKind::String));
            current=joined;
        }
    }
    return current;
}

void ExpressionLowering::materialize_scaled_real(const std::string& name) {
    const auto* binding=scope_.scaled_real(name);
    if(!binding) return;
    const auto integer=Type::simple(TypeKind::Int),real=Type::simple(TypeKind::Real);
    const auto load=[&](const std::string& slot,const Type& type) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,slot,type});return value;
    };
    auto general=load(binding->general,integer),zero=builder_.fresh(),condition=builder_.fresh();
    builder_.emit(ConstantExact{zero,"0",integer});
    builder_.emit(Binary{condition,"!=",general,zero,integer,Type::simple(TypeKind::Bool)});
    const auto fallback=builder_.label("exact.loop.general"),scaled=builder_.label("exact.loop.scaled"),
               done=builder_.label("exact.loop.materialized");
    builder_.emit(Branch{condition,fallback,scaled});
    builder_.enter(builder_.add_block(fallback));
    auto multiplier=load(binding->counter,integer);
    if(binding->exclusive_start) {
        auto one=builder_.fresh(),shifted=builder_.fresh();
        builder_.emit(ConstantExact{one,"1",integer});
        builder_.emit(Binary{shifted,"+",multiplier,one,integer,integer});multiplier=shifted;
    }
    auto value=builder_.fresh();
    builder_.emit(ExactLoopValue{value,multiplier,load(binding->start,real),load(binding->step,real)});
    builder_.emit(StoreLocal{scope_.source_local(name),value,real});
    if(binding->exclusive_start) release_owned(builder_,multiplier,integer);
    builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(scaled));
    const auto numerator_slot=builder_.hidden("exact.loop.use_numerator");scope_.local_type(numerator_slot)=integer;
    auto machine=load(binding->machine_flag,integer),use_machine=builder_.fresh();
    builder_.emit(Binary{use_machine,"!=",machine,zero,integer,Type::simple(TypeKind::Bool)});
    const auto fast=builder_.label("exact.loop.machine_value"),wide=builder_.label("exact.loop.integer_value"),
               available=builder_.label("exact.loop.numerator_ready");
    builder_.emit(Branch{use_machine,fast,wide});builder_.enter(builder_.add_block(fast));
    auto numerator=bare_from_int64(load(binding->machine_numerator,Type::simple(TypeKind::Int64)),integer,false);
    builder_.emit(StoreLocal{numerator_slot,numerator,integer});builder_.emit(Jump{available});
    builder_.enter(builder_.add_block(wide));
    numerator=lifetime_.copy_value(load(binding->numerator,integer),integer);
    builder_.emit(StoreLocal{numerator_slot,numerator,integer});builder_.emit(Jump{available});
    builder_.enter(builder_.add_block(available));
    value=builder_.fresh();
    builder_.emit(ExactFromScaled{value,load(numerator_slot,integer),load(binding->denominator,integer)});
    builder_.emit(StoreLocal{scope_.source_local(name),value,real});
    auto used=builder_.fresh();builder_.emit(LoadLocal{used,numerator_slot,integer,true});release_owned(builder_,used,integer);
    builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(done));
}

ValueId ExpressionLowering::lower_name(const Expr& e, const NameExpr& n) {
    if (n.self_reference) return load_receiver(builder_, enclosing_class_);
    materialize_scaled_real(n.name);
    if(is_builtin_text_constant(n.name)){auto out=builder_.fresh();builder_.emit(ConstantString{out,std::string(builtin_text_constant(n.name))});return out;}
    if(const auto it=checked_.field_accesses.find(&e);it!=checked_.field_accesses.end()){
        auto object=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
        initialization_flags_.check_field(e,object);
        if(checked_.classes.at(it->second.owner).fields.at(it->second.index).reference) {
            auto address=builder_.fresh();builder_.emit(FieldReferenceAddress{address,object,it->second.index});builder_.emit(LoadAddress{out,address,it->second.type});
        } else builder_.emit(FieldGet{out,object,it->second.index,it->second.type});return out;
    }
    if (const auto function = checked_.function_references.find(&e);
        function != checked_.function_references.end()) {
        auto out=builder_.fresh();
        builder_.emit(FunctionRef{out,function->second,checked_.raw_types.at(&e)});
        return out;
    }
    if(const auto* value=scope_.source_value(n.name)) return *value;
    auto out=builder_.fresh(); const auto raw=checked_.raw_types.at(&e);
    if(const auto* bit=scope_.bit_place(n.name)) {
        builder_.emit(BinGet{out,bit->array,bit->index,{0,0},true});
        return out;
    }
    if(scope_.is_source_reference(n.name)){
        const auto& ir_name=scope_.source_reference(n.name); const auto t=scope_.reference_type_at(ir_name);
        builder_.emit(LoadReference{out,ir_name,t});
        if(t.kind==TypeKind::Union&&raw.kind!=TypeKind::Union){auto pv=builder_.fresh();builder_.emit(VariantPayload{pv,out,raw});return pv;}
        return out;
    }
    const auto& ir_name=scope_.source_local(n.name); auto t=scope_.local_type_at(ir_name);
    if (t.kind == TypeKind::Tensor && raw.kind == TypeKind::Tensor) {
        t.length = raw.length;
        t.tensor_known_shape_prefix = raw.tensor_known_shape_prefix;
    }
    builder_.emit(LoadLocal{out,ir_name,t}); if(t.kind==TypeKind::Union&&raw.kind!=TypeKind::Union){auto pv=builder_.fresh();builder_.emit(VariantPayload{pv,out,raw});return pv;}return out;
}

ValueId ExpressionLowering::lower_member(const Expr& e, const MemberExpr& n) {
    if (checked_.tensor_grad_accesses.contains(&e)) return autograd_.lower_tensor_grad(e, n);
    if (const auto member = checked_.error_members.find(&e); member != checked_.error_members.end())
        return errors_.lower_member(e, n, member->second, checked_.raw_types.at(&e));
    if (const auto part = checked_.complex_parts.find(&e); part != checked_.complex_parts.end()) {
        // z.real and z.imag: a part, borrowed from a com box as a field is
        // from its object.
        const bool base_owned=lifetime_.expression_owns_result(*n.base);
        const auto complex=type_of(checked_,*n.base);
        const auto part_type=complex_part_type(complex);
        auto value=lower(*n.base),out=builder_.fresh();
        builder_.emit(ComplexPart{out,value,static_cast<std::uint32_t>(part->second),complex});
        if(base_owned){
            if(owns_managed(part_type)) out=lifetime_.copy_value(out,part_type);
            release_owned(builder_,value,complex);
        }
        return out;
    }
    const bool base_owned=lifetime_.expression_owns_result(*n.base);
    auto object=lower(*n.base),out=builder_.fresh();
    const auto& info=checked_.field_accesses.at(&e);
    initialization_flags_.check_field(e,object);
    if(checked_.classes.at(info.owner).fields.at(info.index).reference) {
        auto address=builder_.fresh();builder_.emit(FieldReferenceAddress{address,object,info.index});builder_.emit(LoadAddress{out,address,info.type});
    } else builder_.emit(FieldGet{out,object,info.index,info.type});
    if(base_owned){
        if(owns_managed(info.type)) out=lifetime_.copy_value(out,info.type);
        release_owned(builder_,object,type_of(checked_,*n.base));
    }
    return out;
}

ValueId ExpressionLowering::lower_array_comprehension(const Expr& e,
                                                       const ArrayComprehensionExpr& n) {
    auto before = scope_.snapshot_full();
    const auto array_type = type_of(checked_, *n.source);
    const auto item_type = *array_type.first;
    const auto result_type = checked_.raw_types.at(&e);
    const auto integer = Type::simple(TypeKind::Int64);
    const auto line = static_cast<std::uint32_t>(n.source->span.start.line);
    const auto column = static_cast<std::uint32_t>(n.source->span.start.column);
    const auto source_name = builder_.hidden("generation.source");
    const auto result_name = builder_.hidden("generation.result");
    const auto index_name = builder_.hidden("generation.index");
    const auto item_name = scope_.bind_source_local(builder_, n.name, item_type);
    // Owned declared slots are part of ordinary return/report cleanup, even
    // when a source, condition or value propagates an error midway through.
    builder_.emit(DeclareLocal{source_name, array_type, "", 0, 0});
    builder_.emit(DeclareLocal{result_name, result_type, "", 0, 0});
    builder_.emit(DeclareLocal{item_name, item_type, n.name,
        static_cast<std::uint32_t>(n.binding_span.start.line),
        static_cast<std::uint32_t>(n.binding_span.start.column)});
    scope_.local_type(source_name) = array_type;
    scope_.local_type(result_name) = result_type;
    scope_.local_type(index_name) = integer;
    auto source = lower_into(*n.source, array_type);
    builder_.emit(StoreLocal{source_name, source, array_type});
    // A borrowed input becomes an owned value copy at entry. Its copy-on-
    // write snapshot cannot be replaced by calls in the generated body.
    auto length = builder_.fresh(); builder_.emit(ArrayLength{length, source});
    auto result = builder_.fresh(); builder_.emit(ArrayMake{result, {}, result_type});
    builder_.emit(StoreLocal{result_name, result, result_type});
    auto zero = builder_.const_int(0); builder_.emit(StoreLocal{index_name, zero, integer});
    const auto cond = builder_.label("generation.cond"), body = builder_.label("generation.body"),
               selected = builder_.label("generation.selected"), step = builder_.label("generation.step"),
               done = builder_.label("generation.done");
    builder_.emit(Jump{cond}); builder_.enter(builder_.add_block(cond));
    auto index = builder_.fresh(), more = builder_.fresh();
    builder_.emit(LoadLocal{index, index_name, integer});
    builder_.emit(Binary{more, "<", index, length, integer, Type::simple(TypeKind::Bool)});
    builder_.emit(Branch{more, body, done}); builder_.enter(builder_.add_block(body));
    auto element = builder_.fresh();
    builder_.emit(ArrayGet{element, source, index, item_type, {line, column}, false, true});
    element = lifetime_.copy_value(element, item_type);
    builder_.emit(StoreLocal{item_name, element, item_type});
    if (n.condition) {
        auto accepted = lower_into(*n.condition, Type::simple(TypeKind::Bool));
        builder_.emit(Branch{accepted, selected, step});
    } else builder_.emit(Jump{selected});
    builder_.enter(builder_.add_block(selected));
    auto value = lower_into(*n.value, *result_type.first);
    auto current = builder_.fresh(), old_length = builder_.fresh(), grown = builder_.fresh();
    builder_.emit(LoadLocal{current, result_name, result_type});
    builder_.emit(ArrayLength{old_length, current});
    // The result is a hidden, fully initialized, unaliased Array. The value
    // was evaluated before growing it and cannot retain its storage.
    builder_.emit(ArrayGrowMove{grown, current, result_type});
    builder_.emit(StoreLocal{result_name, grown, result_type, false, true});
    builder_.emit(ArraySet{grown, old_length, value, *result_type.first,
        {static_cast<std::uint32_t>(n.value->span.start.line),
        static_cast<std::uint32_t>(n.value->span.start.column)}, true, true});
    builder_.emit(Jump{step}); builder_.enter(builder_.add_block(step));
    auto consumed = builder_.fresh(); builder_.emit(LoadLocal{consumed, item_name, item_type, true});
    release_owned(builder_, consumed, item_type);
    auto one = builder_.const_int(1), next = builder_.fresh();
    builder_.emit(Binary{next, "+", index, one, integer, integer, 0, 0, true});
    builder_.emit(StoreLocal{index_name, next, integer}); builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(done));
    auto out = builder_.fresh(); builder_.emit(LoadLocal{out, result_name, result_type, true});
    auto old_source = builder_.fresh(); builder_.emit(LoadLocal{old_source, source_name, array_type, true});
    release_owned(builder_, old_source, array_type);
    scope_.restore(std::move(before));
    return out;
}

ValueId ExpressionLowering::lower_array_literal(const Expr& e, const ArrayExpr& n) {
    std::vector<ValueId> values; values.reserve(n.elements.size());
    for(const auto& x:n.elements) values.push_back(lower_into(*x,type_of(checked_,*x)));
    auto out=builder_.fresh(); builder_.emit(ArrayMake{out,std::move(values),checked_.raw_types.at(&e)}); return out;
}

ValueId ExpressionLowering::lower_index(const Expr& e, const IndexExpr& n) {
    const bool snapshot_base=lifetime_.index_base_needs_snapshot(n);
    const bool base_owned=snapshot_base || lifetime_.expression_owns_result(*n.base);
    const auto base_type=type_of(checked_,*n.base);
    const bool standard_collection_array =
        base_type.kind == TypeKind::Array &&
        enclosing_class_.is_standard_collection() &&
        checked_.field_accesses.contains(n.base.get());
    const bool initialization_proven =
        base_type.kind == TypeKind::Array &&
        (facts_.array_initialization.fully_initialized(*n.base) ||
         standard_collection_array);
    auto a=lower(*n.base);
    // Evaluate the base before the indices without leaving a borrowed
    // pointer exposed to replacement or in-place mutation in later calls.
    if (snapshot_base) a=lifetime_.copy_value(a,base_type);
    const bool hold_base = base_owned && std::any_of(
        n.items.begin(), n.items.end(), [](const IndexPart& item) {
            const auto runs_code = [](const ExprPtr& operand) {
                return operand && expression_runs_code(*operand);
            };
            return runs_code(item.index) || runs_code(item.start) ||
                   runs_code(item.stop) || runs_code(item.step);
        });
    std::string base_slot;
    if (hold_base) {
        // Both snapshots and existing owned temporaries need ordinary
        // return cleanup if an index operand propagates an error.
        base_slot = builder_.hidden("index.base");
        builder_.emit(DeclareLocal{base_slot, base_type, "", 0, 0});
        scope_.local_type(base_slot) = base_type;
        builder_.emit(StoreLocal{base_slot, a, base_type});
    }
    const auto release_base = [&] {
        if (hold_base) {
            auto consumed = builder_.fresh();
            builder_.emit(LoadLocal{consumed, base_slot, base_type, true});
            release_owned(builder_, consumed, base_type);
        } else if (base_owned) release_owned(builder_, a, base_type);
    };
    if (base_type.kind == TypeKind::Array && n.items.size() == 1 &&
        !n.items.front().slice && n.items.front().index &&
        type_of(checked_, *n.items.front().index).kind == TypeKind::Array) {
        // Indexed gather and boolean masks are read-only selection operators.
        // Each operand is evaluated exactly once and the output has separate
        // storage; managed rows receive value copies.
        const auto& selector_expression = *n.items.front().index;
        const auto selector_type = type_of(checked_, selector_expression);
        const auto selector_element = *selector_type.first;
        const bool is_mask = selector_element.kind == TypeKind::Bool;
        const auto output_type = checked_.raw_types.at(&e);
        const auto element_type = *base_type.first;
        const auto integer = Type::simple(TypeKind::Int64);
        const auto boolean = Type::simple(TypeKind::Bool);
        const SourcePosition site{
            static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)};
        auto selector = lower(selector_expression);
        auto selector_length = builder_.fresh();
        builder_.emit(ArrayLength{selector_length, selector});
        auto source_length = builder_.fresh();
        builder_.emit(ArrayLength{source_length, a});
        if (is_mask)
            builder_.emit(ExtentEqualCheck{selector_length, source_length, site});
        auto index_slot = builder_.hidden("selection.index");
        scope_.local_type(index_slot) = integer;
        auto zero = builder_.const_int(0);
        auto one = builder_.const_int(1);
        auto output_length = selector_length;
        if (is_mask) {
            // A mask has dynamic output size: count set bits before allocation.
            auto count_slot = builder_.hidden("selection.count");
            scope_.local_type(count_slot) = integer;
            builder_.emit(StoreLocal{count_slot, zero, integer});
            builder_.emit(StoreLocal{index_slot, zero, integer});
            auto cond = builder_.label("selection.count.cond");
            auto body = builder_.label("selection.count.body");
            auto accept = builder_.label("selection.count.accept");
            auto step = builder_.label("selection.count.step");
            auto done = builder_.label("selection.count.done");
            builder_.emit(Jump{cond}); builder_.enter(builder_.add_block(cond));
            auto i = builder_.fresh(), less = builder_.fresh();
            builder_.emit(LoadLocal{i, index_slot, integer});
            builder_.emit(Binary{less, "<", i, selector_length, integer, boolean});
            builder_.emit(Branch{less, body, done}); builder_.enter(builder_.add_block(body));
            auto flag = builder_.fresh();
            builder_.emit(ArrayGet{flag, selector, i, boolean, site, false, true});
            builder_.emit(Branch{flag, accept, step});
            builder_.enter(builder_.add_block(accept));
            auto count = builder_.fresh(), incremented = builder_.fresh();
            builder_.emit(LoadLocal{count, count_slot, integer});
            builder_.emit(Binary{incremented, "+", count, one, integer, integer, 0, 0, true});
            builder_.emit(StoreLocal{count_slot, incremented, integer});
            builder_.emit(Jump{step}); builder_.enter(builder_.add_block(step));
            auto next = builder_.fresh();
            builder_.emit(Binary{next, "+", i, one, integer, integer, 0, 0, true});
            builder_.emit(StoreLocal{index_slot, next, integer});
            builder_.emit(Jump{cond}); builder_.enter(builder_.add_block(done));
            output_length = builder_.fresh();
            builder_.emit(LoadLocal{output_length, count_slot, integer});
        }
        auto output = builder_.fresh();
        builder_.emit(ArrayAlloc{output, output_length, output_type});
        builder_.emit(StoreLocal{index_slot, zero, integer});
        const auto write_slot = builder_.hidden("selection.write");
        if (is_mask) {
            scope_.local_type(write_slot) = integer;
            builder_.emit(StoreLocal{write_slot, zero, integer});
        }
        const auto cond = builder_.label("selection.gather.cond");
        const auto body = builder_.label("selection.gather.body");
        const auto selected = builder_.label("selection.gather.selected");
        const auto step = builder_.label("selection.gather.step");
        const auto done = builder_.label("selection.gather.done");
        builder_.emit(Jump{cond}); builder_.enter(builder_.add_block(cond));
        auto i = builder_.fresh(), less = builder_.fresh();
        builder_.emit(LoadLocal{i, index_slot, integer});
        builder_.emit(Binary{less, "<", i, selector_length, integer, boolean});
        builder_.emit(Branch{less, body, done}); builder_.enter(builder_.add_block(body));
        auto selection = builder_.fresh();
        builder_.emit(ArrayGet{selection, selector, i, selector_element, site, false, true});
        ValueId input_index = i;
        if (is_mask) {
            builder_.emit(Branch{selection, selected, step});
            builder_.enter(builder_.add_block(selected));
        } else if (selector_element.kind != TypeKind::Int64) {
            input_index = builder_.fresh();
            builder_.emit(NumericConvert{input_index, selection, selector_element,
                                        integer, false, site});
        } else input_index = selection;
        auto item = builder_.fresh();
        builder_.emit(ArrayGet{item, a, input_index, element_type, site});
        item = lifetime_.copy_value(item, element_type);
        auto write_index = i;
        if (is_mask) {
            write_index = builder_.fresh();
            builder_.emit(LoadLocal{write_index, write_slot, integer});
        }
        builder_.emit(ArraySet{output, write_index, item, element_type, site, false, true});
        if (is_mask) {
            auto next_write = builder_.fresh();
            builder_.emit(Binary{next_write, "+", write_index, one, integer, integer, 0, 0, true});
            builder_.emit(StoreLocal{write_slot, next_write, integer});
            builder_.emit(Jump{step}); builder_.enter(builder_.add_block(step));
        }
        auto next = builder_.fresh();
        builder_.emit(Binary{next, "+", i, one, integer, integer, 0, 0, true});
        builder_.emit(StoreLocal{index_slot, next, integer});
        builder_.emit(Jump{cond}); builder_.enter(builder_.add_block(done));
        lifetime_.release_temporary(selector_expression, selector);
        release_base();
        return output;
    }
    if(base_type.kind==TypeKind::String && n.items.front().slice) {
        auto out=lower_string_slice(e,n,a,false,base_type);
        release_base();
        return out;
    }
    auto out=builder_.fresh();
    if(base_type.kind==TypeKind::Tensor){
        if (n.items.size() == 1 && !n.items.front().slice &&
            n.items.front().index &&
            type_of(checked_, *n.items.front().index).kind == TypeKind::Tensor) {
            const auto& selector = *n.items.front().index;
            auto mask = lower(selector);
            TensorIndexPart marked_mask;
            marked_mask.kind = static_cast<std::uint8_t>(abi::tensor_index_spec::mask);
            marked_mask.index = mask;
            std::vector<TensorIndexPart> items;
            items.push_back(std::move(marked_mask));
            builder_.emit(TensorIndex{
                out,a,std::move(items),checked_.raw_types.at(&e),
                {static_cast<std::uint32_t>(e.span.start.line),
                 static_cast<std::uint32_t>(e.span.start.column)}});
            lifetime_.release_temporary(selector, mask);
        } else {
        std::vector<TensorIndexPart> items;
        items.reserve(n.items.size());
        for(const auto& item:n.items){
            namespace spec=abi::tensor_index_spec;
            TensorIndexPart lowered;
            // start:<end (either side omitted) is the half-open slice; any
            // other form is marked and planned by the slice bound rules.
            const bool half_open=
                (item.start_form==SliceStart::inclusive || item.start_form==SliceStart::omitted) &&
                (item.end_form==SliceEnd::exclusive_ascending || item.end_form==SliceEnd::omitted);
            lowered.kind=static_cast<std::uint8_t>(
                !item.slice ? spec::index
                : half_open ? spec::slice
                : spec::marked_slice_kind(item.start_form,item.end_form));
            if(item.index) lowered.index=lower_int64(*item.index);
            if(item.start) lowered.start=lower_int64(*item.start);
            if(item.stop) lowered.stop=lower_int64(*item.stop);
            if(item.step) lowered.step=lower_int64(*item.step);
            items.push_back(std::move(lowered));
        }
        builder_.emit(TensorIndex{
            out,a,std::move(items),base_type,
            {static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)}});
        }
    }else{
        if(base_type.kind==TypeKind::Bin) {
            const auto& item=n.items.front();
            const bool half_open=
                (item.start_form==SliceStart::inclusive || item.start_form==SliceStart::omitted) &&
                (item.end_form==SliceEnd::exclusive_ascending || item.end_form==SliceEnd::omitted);
            if(item.slice && !half_open){
                // Every other form plans the bits it selects.
                builder_.emit(BinSlicePlanned{out,a,lower_slice_bounds(e,item),
                    {static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)}});
            }else if(item.slice){
                auto start=item.start?lower_int64(*item.start):builder_.const_int(0);
                ValueId stop;
                if(item.stop) stop=lower_int64(*item.stop);
                else { stop=builder_.fresh(); builder_.emit(BinLength{stop,a}); }
                // A slice reports the indexing expression.
                builder_.emit(BinSlice{out,a,start,stop,
                    {static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)}});
            }else{
                // An element index failure reports the index operand.
                auto i=lower_int64(*item.index);
                builder_.emit(BinGet{
                    out,a,i,{static_cast<std::uint32_t>(item.index->span.start.line),
                    static_cast<std::uint32_t>(item.index->span.start.column)},
                    checked_.bounds_proven.contains(item.index.get())});
            }
        } else if(base_type.kind==TypeKind::Array && n.items.front().slice) {
            // A slice reports the indexing expression; its result is a new
            // array of copies of the selected elements.
            auto bounds=lower_slice_bounds(e,n.items.front());
            builder_.emit(ArraySlice{
                out,a,std::move(bounds),base_type,
                {static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)},
                false,initialization_proven});
        } else if(base_type.kind==TypeKind::String) {
            const auto& index=*n.items.front().index;
            auto i=lower_int64(index);
            builder_.emit(StringIndex{
                out,a,i,{static_cast<std::uint32_t>(index.span.start.line),
                static_cast<std::uint32_t>(index.span.start.column)}});
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
                {static_cast<std::uint32_t>(n.items.front().index->span.start.line),
                static_cast<std::uint32_t>(n.items.front().index->span.start.column)},
                initialization_proven, bounds_proven,
                initialization_guard, bounds_guard});
            if(base_owned && owns_managed(checked_.raw_types.at(&e)))
                out=lifetime_.copy_value(out,checked_.raw_types.at(&e));
        }
    }
    release_base();
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
        out,n.op,v,type_of(checked_,e),{static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)},
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
    ValueId propagated=0;
    if(const auto call=container_owned?errors_.hop_call(*n.value):std::nullopt){
        // The error leaves the call: a hop, which takes it out of the cell.
        auto hopped=errors_.hop(container,src,*call,abi::ErrorHopKind::try_call);
        propagated=conversions_.convert(hopped,error_type,builder_.function()->result,false);
    }else{
        auto payload=builder_.fresh();
        builder_.emit(VariantPayload{payload,container,error_type});
        propagated=conversions_.convert(payload,error_type,builder_.function()->result,container_owned);
    }
    if(container_owned) release_owned(builder_,container,src);
    if (enclosing_class_.in_constructor()) {
        auto receiver = load_receiver(builder_, enclosing_class_);
        release_owned(builder_, receiver, Type::class_type(enclosing_class_.name()));
    }
    lifetime_.release_loop_sources();
    builder_.emit(Return{propagated,builder_.function()->result});
    builder_.enter(builder_.add_block(ok));
    if(result.kind!=TypeKind::Union){
        auto out=builder_.fresh();
        builder_.emit(VariantPayload{out,container,result});
        if(container_owned){
            if(owns_managed(result)) out=lifetime_.copy_value(out,result);
            release_owned(builder_,container,src);
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
        if(container_owned) release_owned(builder_,container,src);
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
