// Call lowering: arguments matched to parameters (named, positional,
// defaults), passed by address, borrowed or as owned values (a borrowed
// argument the call may write is passed as a copy, ArgumentIsolation), and
// the borrowed temporaries and copies released after the call; class values built with
// their defaults; calls, by their resolution (a function value, a method
// of the enclosing class, a numeric cast, a constructor, a builtin, a
// function); method calls, dispatched by receiver to the domain that owns
// the method (text_lowering.cpp, tensor_lowering.cpp, ...), and user
// methods.
//
// A user method call evaluates its receiver before its arguments when the
// receiver runs code (a call in it), as source order requires. A receiver
// that is a place (a binding, a field, an element) is read after the
// arguments, at the call, so that the call reaches its storage as it is
// then; reading it there or first cannot be told apart otherwise.

#include "lowering/checked_types.hpp"
#include "quidra/standard_classes.hpp"
#include "lowering/lowerer.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include "quidra/member_function_names.hpp"

namespace quidra::lowering {

// A fresh class receiver with every declared default evaluated and every
// other field uninitialized, used while executing class generation. Standard
// collection bare declarations may request their compiler-owned initializer.
ValueId CallLowering::make_class_with_defaults(const std::string& class_name, bool initialize_standard){
    const auto& info=checked_.classes.at(class_name);
    if (initialize_standard &&
        (standard_class::is_map_instance(class_name, info.standard_library) ||
         standard_class::is_set_instance(class_name, info.standard_library)) &&
        info.constructors.size() == 1) {
        const auto& initializer = info.constructors.front();
        auto out = builder_.fresh();
        builder_.emit(Call{out, initializer, {}, Type::class_type(class_name)});
        return out;
    }
    std::vector<std::optional<ValueId>> fields(info.fields.size());
    for(const auto& field:info.fields){
        if(!field.default_value)continue;
        fields[field.index]=lowerer_.lower_into(*field.default_value,field.type);
    }
    auto out=builder_.fresh();
    builder_.emit(ClassMake{out,Type::class_type(class_name),std::move(fields)});
    return out;
}

namespace {

// Whether a parameter of an extern is a by-value const string, bin or
// tensor: a read-only borrow, passed by address (FunctionLowering gives it
// the parameter form of `const T &`).
bool is_foreign_borrow(const FunctionParameterType& parameter) {
    return !parameter.writable && parameter.is_const &&
           (parameter.type.kind == TypeKind::String || parameter.type.kind == TypeKind::Bin ||
            parameter.type.kind == TypeKind::Tensor || is_foreign_tensor_array(parameter.type));
}

} // namespace

// Whether `expression` names storage whose address lower_address forms: a
// local or reference, a field, or an element of an array.
bool CallLowering::foreign_borrow_place(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (checked_.field_accesses.contains(&expression)) return true;
        return !name->this_qualifier &&
               (scope_.is_source_reference(name->name) || scope_.has_source_local(name->name));
    }
    if (std::holds_alternative<MemberExpr>(expression.data))
        return checked_.field_accesses.contains(&expression);
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        return index->items.size() == 1 && !index->items.front().slice &&
               index->items.front().index &&
               type_of(checked_, *index->base).kind == TypeKind::Array;
    }
    return false;
}

CallLowering::LoweredCallArguments CallLowering::lower_call_arguments(const std::vector<CallArg>& source_args,
                                           const FunctionType& sig,
                                           std::size_t offset,
                                           const std::string& callee) {
    std::unordered_map<std::string,std::size_t> index;
    for(std::size_t i=offset;i<sig.parameters.size();++i) index[sig.parameters[i].name]=i;
    LoweredCallArguments lowered;
    lowered.args.resize(sig.parameters.size());
    std::vector<bool> filled(sig.parameters.size());
    for(std::size_t i=0;i<offset;++i) filled[i]=true;
    std::size_t positional=offset;
    const auto lower_value = [&](const Expr& expression, const FunctionParameterType& parameter, std::size_t target) {
        if (!borrows_.parameter_is_borrowed(callee, target)) return lowerer_.lower_into(expression, parameter.type);
        const bool exact = checked_.raw_types.at(&expression) == parameter.type && type_of(checked_, expression) == parameter.type;
        const auto value = exact ? lowerer_.lower(expression) : lowerer_.lower_into(expression, parameter.type);
        if (!exact || lifetime_.expression_owns_result(expression)) {
            lowered.borrowed_temporaries.push_back({value, parameter.type});
            return value;
        }
        // Storage the call may write keeps its value at the call in a copy.
        if (isolation_.copies(expression)) {
            const auto copy = lifetime_.copy_value(value, parameter.type);
            lowered.borrowed_temporaries.push_back({copy, parameter.type});
            return copy;
        }
        return value;
    };
    // A const string, bin or tensor parameter of an extern is a read-only
    // borrow that reaches C through the address of storage holding the
    // value (D12), exactly as the spelling `const T &` passed `&storage`:
    // a variable, field or array element of the parameter's type is passed
    // by its address, and any other value, or one the call may write
    // through another argument, is held in a hidden local for the call.
    const auto lower_foreign_borrow = [&](const Expr& expression, const FunctionParameterType& parameter) {
        if (type_of(checked_, expression) == parameter.type &&
            checked_.raw_types.at(&expression) == parameter.type &&
            foreign_borrow_place(expression) && !isolation_.copies(expression))
            return lowerer_.lower_address(expression, false);
        auto value = lowerer_.lower_into(expression, parameter.type);
        if (lifetime_.expression_owns_result(expression) ||
            type_of(checked_, expression) != parameter.type ||
            checked_.raw_types.at(&expression) != parameter.type) {
            lowered.borrowed_temporaries.push_back({value, parameter.type});
        } else if (isolation_.copies(expression)) {
            value = lifetime_.copy_value(value, parameter.type);
            lowered.borrowed_temporaries.push_back({value, parameter.type});
        }
        const auto slot = builder_.hidden("ffi.borrow");
        scope_.local_type(slot) = parameter.type;
        builder_.emit(StoreLocal{slot, value, parameter.type, true});
        const auto address = builder_.fresh();
        builder_.emit(AddressLocal{address, slot});
        return address;
    };
    for(const auto& arg:source_args){
        std::size_t target;
        if(arg.name) target=index.at(*arg.name);
        else { while(positional<filled.size()&&filled[positional]) ++positional; target=positional++; }
        filled[target]=true; const auto& param=sig.parameters[target];
        if(param.writable) lowered.args[target]=CallArgument{0,lowerer_.lower_address(*arg.value, !param.is_const)};
        else if(sig.external && is_foreign_borrow(param))
            lowered.args[target]=CallArgument{0,lower_foreign_borrow(*arg.value,param)};
        else lowered.args[target]=CallArgument{lower_value(*arg.value,param,target),std::nullopt};
    }
    for(std::size_t i=offset;i<filled.size();++i) if(!filled[i]){
        const auto& parameter=sig.parameters[i];
        lowered.args[i]=CallArgument{lower_value(*parameter.default_value,parameter,i),std::nullopt};
    }
    return lowered;
}

void CallLowering::release_borrowed_temporaries(const LoweredCallArguments& lowered) {
    for (const auto& [value, type] : lowered.borrowed_temporaries)
        release_owned(builder_,value, type);
}


CallLowering::ShapedCall CallLowering::prepare_shaped_call(
    const Expr& expression, const std::string& callee, const FunctionType& signature,
    LoweredCallArguments& arguments) {
    ShapedCall result; result.callee = callee;
    for (const auto& source : checked_.program.functions)
        if (source.name == callee) result.declaration = &source;
    for (const auto& source : checked_.program.classes) {
        std::size_t constructor = 0;
        for (const auto& method : source.methods) {
            const auto name = method.is_constructor
                ? member_function_name::constructor(source.name, constructor++)
                : member_function_name::method(source.name, method.name);
            if (name == callee) {
                result.declaration = &method;
                result.owner = source.name;
                result.parameter_offset = method.is_constructor ? 0 : 1;
            }
        }
    }
    if (!result.declaration) return result;
    const auto unit = checked_.signature_shapes.find(result.declaration);
    if (unit == checked_.signature_shapes.end()) return result;
    result.external = result.declaration->external_symbol.has_value();
    const bool force = lowerer_.options().check_every_shape_constraint;
    const auto complete = checked_.complete_signature_checks.find(&expression);
    bool checked_entry = !result.external && !force &&
        complete != checked_.complete_signature_checks.end() && complete->second &&
        std::any_of(unit->second.parts.begin(), unit->second.parts.end(),
                    [](const auto& part) { return part.parameter != 0; });
    for (const auto& parameter : result.declaration->parameters)
        if (!parameter.type.dimension_expressions.empty()) checked_entry = false;
    if (!result.external && !checked_entry) return result;
    auto scope = scope_.snapshot_full();
    const auto isolated = lowerer_.isolate_shape_source_facts(true);
    scope_.reset();
    for (std::size_t p = 0; p < signature.parameters.size(); ++p) {
        const auto& parameter = signature.parameters[p];
        const auto& argument = arguments.args[p];
        auto value = argument.value;
        if (argument.writable_address) {
            value = builder_.fresh();
            builder_.emit(LoadAddress{value, *argument.writable_address, parameter.type});
        }
        if (argument.writable_address) {
            const auto reference = scope_.bind_source_reference(builder_, parameter.name, parameter.type);
            builder_.emit(DeclareReference{reference, parameter.type});
            builder_.emit(BindReference{reference, *argument.writable_address});
        } else {
            const auto local = scope_.bind_source_local(builder_, parameter.name, parameter.type);
            builder_.emit(DeclareLocal{local, parameter.type, {}, 0, 0});
            builder_.emit(StoreLocal{local, value, parameter.type, true});
        }
        if (p >= result.parameter_offset) result.values.push_back(value);
    }
    std::string receiver;
    bool receiver_reference = false;
    if (result.parameter_offset) {
        receiver_reference = scope_.is_source_reference("$receiver");
        receiver = receiver_reference ? scope_.source_reference("$receiver") : scope_.source_local("$receiver");
    }
    auto shape_class = lowerer_.shape_class_context(result.owner, receiver, receiver_reference);
    const auto& plan = checked_.shape_plans.at(unit->second.source.get());
    const auto proof = checked_.proven_shape_constraints.find(&expression);
    result.captures = shape_constraints_.capture_signature_shape(
        unit->second, plan, result.values, expression.span,
        !force && proof != checked_.proven_shape_constraints.end() ? &proof->second.parameters : nullptr);
    if (checked_entry) {
        std::unordered_set<std::string> captured;
        const auto returned_array = shape_constraints_.capture_extents(
            result.declaration->return_type.dimension_expressions, "call.return.array");
        for (const auto& local : returned_array)
            if (local && captured.insert(*local).second) {
                const auto value = builder_.fresh();
                builder_.emit(LoadLocal{value, *local, Type::simple(TypeKind::Int64)});
                arguments.args.push_back(CallArgument{value, std::nullopt});
            }
        for (const auto& part : result.captures)
            for (const auto& local : part)
                if (local && captured.insert(*local).second) {
                    const auto value = builder_.fresh();
                    builder_.emit(LoadLocal{value, *local, Type::simple(TypeKind::Int64)});
                    arguments.args.push_back(CallArgument{value, std::nullopt});
                }
        for (const auto& variable : plan.variables)
            if (variable.read) {
                const auto& local = scope_.source_local(variable.name);
                if (captured.insert(local).second) {
                    const auto value = builder_.fresh();
                    const auto type = scope_.local_type_at(local);
                    builder_.emit(LoadLocal{value, local, type, owns_managed(type)});
                    arguments.args.push_back(CallArgument{value, std::nullopt});
                }
            }
        result.callee += ".shape_checked";
    }
    scope_.restore(std::move(scope));
    lowerer_.restore_shape_class(std::move(shape_class));
    lowerer_.isolate_shape_source_facts(isolated);
    return result;
}

void CallLowering::finish_shaped_call(
    const Expr& expression, const FunctionType& signature,
    const LoweredCallArguments& arguments, const ShapedCall& shape) {
    if (!shape.external || shape.captures.empty()) return;
    const auto& unit = checked_.signature_shapes.at(shape.declaration);
    for (std::size_t p = 0; p < unit.parts.size(); ++p) {
        const auto& part = unit.parts[p];
        if (part.parameter == 0) continue;
        const auto index = part.parameter - 1 + shape.parameter_offset;
        const auto& parameter = signature.parameters[index];
        if (!parameter.writable || parameter.is_const) continue;
        const auto value = builder_.fresh();
        builder_.emit(LoadAddress{value, *arguments.args[index].writable_address, parameter.type});
        shape_constraints_.emit_shaped_constraint(value, TypeKind::Tensor,
            shape.captures[p], expression.span);
    }
}

// The methods of a type name come first (int.parse, bin.fill,
// string.from_utf8, string.repeat), then the methods of the receiver's type,
// the implicit .string() and last a user method, in this order. Each try_
// function returns nullopt when the call is not one of its methods; one that
// lowered the receiver before finding no method of that name leaves the
// receiver lowered, and the chain goes on.
ValueId CallLowering::lower_method_call(const Expr& e, const MethodCallExpr& n) {
    if (const auto out = conversions_.try_parse_method(e, n)) return *out;
    if (const auto out = bin_.try_bin_type_method(n)) return *out;
    if (const auto out = text_.try_string_type_method(e, n)) return *out;
    const auto receiver_type=type_of(checked_,*n.receiver);
    if (const auto out = autograd_.try_autograd_target_method(e, n, receiver_type)) return *out;
    if (const auto out = files_.try_file_handle_method(e, n, receiver_type)) return *out;
    if (const auto out = text_.try_string_method(e, n, receiver_type)) return *out;
    if (const auto out = tensors_.try_tensor_method(e, n, receiver_type)) return *out;
    if (const auto out = conversions_.try_error_method(n, receiver_type)) return *out;
    if (const auto out = aggregates_.try_array_method(e, n, receiver_type)) return *out;
    if (const auto out = text_.try_implicit_string_method(e, n)) return *out;
    return lower_user_method_call(e, n);
}

ValueId CallLowering::lower_user_method_call(const Expr& e, const MethodCallExpr& n) {
    const auto internal=checked_.method_calls.at(&e).internal_name;
    const auto& sig=checked_.functions.at(internal);
    const bool receiver_first=expression_runs_code(*n.receiver);
    ValueId receiver{};
    if(receiver_first) receiver=lowerer_.lower(*n.receiver);
    auto lowered=lower_call_arguments(n.args,sig,1,internal);
    if(!receiver_first) receiver=lowerer_.lower(*n.receiver);
    lowered.args[0]=CallArgument{receiver,std::nullopt};
    const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:builder_.fresh();
    const auto shape = prepare_shaped_call(e, internal, sig, lowered);
    builder_.emit(Call{out,shape.callee,lowered.args,sig.result,{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)},sig.no_normal_return});
    if (sig.result.kind != TypeKind::Never && !sig.no_normal_return)
        finish_shaped_call(e, sig, lowered, shape);
    if(sig.result.kind!=TypeKind::Never && !sig.no_normal_return)
        release_borrowed_temporaries(lowered);
    if(sig.result.kind!=TypeKind::Never && !sig.no_normal_return)
        lifetime_.release_temporary(*n.receiver,receiver);
    return out;
}

ValueId CallLowering::lower_call(const Expr& e, const CallExpr& n) {
    const auto resolution_it=checked_.call_resolutions.find(&e);
    if(resolution_it==checked_.call_resolutions.end()) {
        throw std::logic_error("Checked CallExpr has no call resolution.");
    }
    const auto& resolution=resolution_it->second;

    if (n.callee == "$std.range.generate")
        return lower_bounded_range(e, n);
    if(resolution.kind==CallKind::FunctionValue) return lower_function_value_call(e,n,resolution);
    if(const auto method=checked_.method_calls.find(&e);method!=checked_.method_calls.end()){
        return lower_enclosing_method_call(e,n,resolution);
    }
    if(resolution.kind==CallKind::NumericCast) return conversions_.lower_numeric_cast(e,n,resolution);
    if(resolution.kind==CallKind::Constructor) return lower_constructor_call(e,n,resolution);
    if(resolution.kind==CallKind::Builtin) return builtins_.lower_builtin_call(e,n,resolution);
    if(resolution.kind!=CallKind::Function) {
        throw std::logic_error("Unsupported resolved call kind.");
    }
    return lower_function_call(e,n,resolution);
}

// A standalone colon range materializes its own array; ordinary bracket
// slicing never uses this path. No language-visible range function exists.
ValueId CallLowering::lower_bounded_range(const Expr& e, const CallExpr& n) {
    const auto flag = [&](std::size_t i) {
        return std::get<BoolExpr>(n.args[i].value->data).value;
    };
    const bool start_exclusive = flag(3);
    const bool end_exclusive = flag(4);
    const bool explicit_step = flag(5);
    const bool ascending_marker = flag(6);
    const bool descending_marker = flag(7);
    // Range bounds may use arbitrary-precision int/nat. The materialized
    // element type controls only checked storage conversions, never the
    // precision of iteration and endpoint comparisons.
    const auto integer = Type::simple(TypeKind::Int);
    const auto boolean = Type::simple(TypeKind::Bool);
    const auto array_type = checked_.raw_types.at(&e);
    const auto element_type = *array_type.first;
    const SourcePosition site{
        static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)};
    // Exact integers are managed values. Preserve every endpoint in a
    // hidden owned local and return only a borrowed read, just as integer
    // for-header lowering does. This avoids stealing a caller's storage
    // and keeps the end bound live throughout the loop.
    const auto hold = [&](ValueId value, bool borrowed) {
        const auto slot = builder_.hidden("range.operand");
        scope_.local_type(slot) = integer;
        if (borrowed) value = lifetime_.copy_value(value, integer);
        builder_.emit(StoreLocal{slot, value, integer});
        auto out = builder_.fresh();
        builder_.emit(LoadLocal{out, slot, integer});
        return out;
    };
    const auto bound = [&](const Expr& source) {
        auto value = lowerer_.lower(source);
        const auto from = type_of(checked_, source);
        if (from == integer)
            return hold(value, !lifetime_.expression_owns_result(source));
        auto converted = builder_.fresh();
        builder_.emit(NumericConvert{
            converted, value, from, integer, true, site});
        lifetime_.release_temporary(source, value);
        return hold(converted, false);
    };
    auto start = bound(*n.args[0].value);
    auto stop = bound(*n.args[1].value);
    auto descending = builder_.fresh();
    builder_.emit(Binary{descending, ">", start, stop, integer, boolean});
    ValueId direction = descending;
    if (ascending_marker) direction = builder_.const_bool(false);
    if (descending_marker) direction = builder_.const_bool(true);
    auto plus_one = builder_.fresh();
    auto minus_one = builder_.fresh();
    builder_.emit(ConstantExact{plus_one, "1", integer});
    builder_.emit(ConstantExact{minus_one, "-1", integer});
    plus_one = hold(plus_one, false);
    minus_one = hold(minus_one, false);
    const auto step_slot = builder_.hidden("range.step");
    scope_.local_type(step_slot) = integer;
    if (explicit_step) {
        auto value = bound(*n.args[2].value);
        builder_.emit(StoreLocal{step_slot, lifetime_.copy_value(value, integer), integer});
    } else {
        const auto asc = builder_.label("range.step.asc");
        const auto desc = builder_.label("range.step.desc");
        const auto merge = builder_.label("range.step.merge");
        builder_.emit(Branch{direction, desc, asc});
        builder_.enter(builder_.add_block(desc));
        builder_.emit(StoreLocal{step_slot, lifetime_.copy_value(minus_one, integer), integer});
        builder_.emit(Jump{merge});
        builder_.enter(builder_.add_block(asc));
        builder_.emit(StoreLocal{step_slot, lifetime_.copy_value(plus_one, integer), integer});
        builder_.emit(Jump{merge});
        builder_.enter(builder_.add_block(merge));
    }
    auto step = builder_.fresh();
    builder_.emit(LoadLocal{step, step_slot, integer});
    if (explicit_step) {
        if (!ascending_marker && !descending_marker) {
            auto zero = builder_.fresh();
            builder_.emit(ConstantExact{zero, "0", integer});
            direction = builder_.fresh();
            builder_.emit(Binary{direction, "<", step, zero, integer, boolean});
        }
        // Equal bounds permit either step sign. A reversed marker with no
        // explicit step describes an empty exclusive range, as in slicing.
        auto unequal = builder_.fresh();
        auto directions_differ = builder_.fresh();
        auto opposed = builder_.fresh();
        builder_.emit(Binary{unequal, "!=", start, stop, integer, boolean});
        builder_.emit(Binary{directions_differ, "!=", descending, direction, boolean, boolean});
        builder_.emit(Binary{opposed, "and", unequal, directions_differ, boolean, boolean});
        const auto invalid = builder_.label("range.direction.invalid");
        const auto valid = builder_.label("range.direction.valid");
        builder_.emit(Branch{opposed, invalid, valid});
        builder_.enter(builder_.add_block(invalid));
        const auto problem = builder_.fresh();
        builder_.emit(ErrorMake{problem, 0, "SLICE_STEP",
            {ErrorDetailOperand{std::nullopt,
                "Range step opposes the bound direction.", false}},
            std::nullopt, site});
        builder_.emit(FailError{problem, site});
        builder_.enter(builder_.add_block(valid));
    }
    {
        // ForStepCheck consumes the exact-int word, including boxed steps.
        // Validate against the chosen marker direction; equal descending
        // endpoints must not be mistaken for an ascending progression.
        const auto asc = builder_.label("range.check.asc");
        const auto desc = builder_.label("range.check.desc");
        const auto merge = builder_.label("range.check.end");
        builder_.emit(Branch{direction, desc, asc});
        builder_.enter(builder_.add_block(desc));
        builder_.emit(ForStepCheck{step, true, site});
        builder_.emit(Jump{merge});
        builder_.enter(builder_.add_block(asc));
        builder_.emit(ForStepCheck{step, false, site});
        builder_.emit(Jump{merge});
        builder_.enter(builder_.add_block(merge));
    }
    if (start_exclusive) {
        auto first = builder_.fresh();
        builder_.emit(Binary{first, "+", start, step, integer, integer,
                             site.line, site.column});
        start = hold(first, false);
    }
    const auto output_slot = builder_.hidden("range.output");
    const auto index_slot = builder_.hidden("range.index");
    scope_.local_type(output_slot) = array_type;
    scope_.local_type(index_slot) = integer;
    builder_.emit(DeclareLocal{output_slot, array_type, "", 0, 0});
    auto empty = builder_.fresh();
    builder_.emit(ArrayMake{empty, {}, array_type});
    builder_.emit(StoreLocal{output_slot, empty, array_type});
    builder_.emit(StoreLocal{index_slot, lifetime_.copy_value(start, integer), integer});
    const auto cond = builder_.label("range.cond");
    const auto asc = builder_.label("range.cond.asc");
    const auto desc = builder_.label("range.cond.desc");
    const auto body = builder_.label("range.body");
    const auto done = builder_.label("range.done");
    builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(cond));
    builder_.emit(Branch{direction, desc, asc});
    builder_.enter(builder_.add_block(asc));
    auto ascending_index = builder_.fresh(), more_ascending = builder_.fresh();
    builder_.emit(LoadLocal{ascending_index, index_slot, integer});
    builder_.emit(Binary{
        more_ascending, end_exclusive ? "<" : "<=",
        ascending_index, stop, integer, boolean});
    builder_.emit(Branch{more_ascending, body, done});
    builder_.enter(builder_.add_block(desc));
    auto descending_index = builder_.fresh(), more_descending = builder_.fresh();
    builder_.emit(LoadLocal{descending_index, index_slot, integer});
    builder_.emit(Binary{
        more_descending, end_exclusive ? ">" : ">=",
        descending_index, stop, integer, boolean});
    builder_.emit(Branch{more_descending, body, done});
    builder_.enter(builder_.add_block(body));
    auto index = builder_.fresh();
    builder_.emit(LoadLocal{index, index_slot, integer});
    auto value = index;
    if (element_type != integer) {
        value = builder_.fresh();
        builder_.emit(NumericConvert{
            value, index, integer, element_type, true, site});
    } else {
        // ArraySet consumes the stored value. Keep the iteration local's
        // exact integer alive independently until the next step replaces it.
        value = lifetime_.copy_value(index, integer);
    }
    auto current = builder_.fresh(), length = builder_.fresh(), grown = builder_.fresh();
    builder_.emit(LoadLocal{current, output_slot, array_type});
    builder_.emit(ArrayLength{length, current});
    builder_.emit(ArrayGrowMove{grown, current, array_type});
    builder_.emit(StoreLocal{output_slot, grown, array_type, false, true});
    builder_.emit(ArraySet{grown, length, value, element_type, site, true, true});
    auto next = builder_.fresh();
    builder_.emit(Binary{next, "+", index, step, integer, integer,
                         site.line, site.column});
    builder_.emit(StoreLocal{index_slot, next, integer});
    builder_.emit(Jump{cond});
    builder_.enter(builder_.add_block(done));
    auto result = builder_.fresh();
    builder_.emit(LoadLocal{result, output_slot, array_type, true});
    return result;
}

ValueId CallLowering::lower_function_value_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    auto callee=builder_.fresh();
    Type callable_type;
    if(scope_.is_source_reference(resolution.target)){
        const auto& reference_name=scope_.source_reference(resolution.target);
        callable_type=scope_.reference_type_at(reference_name);
        builder_.emit(
            LoadReference{callee,reference_name,callable_type});
    }else{
        const auto local_name=scope_.source_local(resolution.target);
        callable_type=scope_.local_type_at(local_name);
        builder_.emit(
            LoadLocal{callee,local_name,callable_type});
    }
    std::vector<ValueId> args;
    args.reserve(n.args.size());
    for(std::size_t i=0;i<n.args.size();++i)
        args.push_back(lowerer_.lower_into(*n.args[i].value,callable_type.parameters[i]));
    const auto result=*callable_type.first;
    const auto out=(result.kind==TypeKind::Void||result.kind==TypeKind::Never)?0:builder_.fresh();
    builder_.emit(IndirectCall{
        out,callee,std::move(args),callable_type.parameters,result,
        {static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)}});
    return out;
}

ValueId CallLowering::lower_enclosing_method_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    const auto& sig=checked_.functions.at(resolution.target);
    auto lowered=lower_call_arguments(n.args,sig,1,resolution.target);
    lowered.args[0]=CallArgument{load_receiver(builder_,enclosing_class_),std::nullopt};
    const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:builder_.fresh();
    const auto shape = prepare_shaped_call(e, resolution.target, sig, lowered);
    builder_.emit(Call{out,shape.callee,lowered.args,sig.result,{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)},sig.no_normal_return});
    if (sig.result.kind != TypeKind::Never && !sig.no_normal_return)
        finish_shaped_call(e, sig, lowered, shape);
    if(sig.result.kind!=TypeKind::Never && !sig.no_normal_return)
        release_borrowed_temporaries(lowered);
    return out;
}

ValueId CallLowering::lower_constructor_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    if(resolution.type.kind==TypeKind::Error) {
        return lowerer_.lower_error_constructor(e,n);
    }
    if(resolution.target=="string.repeat"){
        auto count=lowerer_.lower(*n.args[0].value),fill=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
        builder_.emit(StringRepeat{out,count,fill});
        lifetime_.release_temporary(*n.args[1].value,fill);
        return out;
    }
    if(resolution.type.kind==TypeKind::Bin){
        if(resolution.target=="bin.cast"){
            auto value=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(BinConvert{
                out,value,type_of(checked_,*n.args[0].value),Type::simple(TypeKind::Bin),
                {static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)}});
            lifetime_.release_temporary(*n.args[0].value,value);
            return out;
        }
        auto length=lowerer_.lower(*n.args[0].value);
        auto fill=lowerer_.lower(*n.args[1].value);
        auto out=builder_.fresh();
        builder_.emit(BinAlloc{out,length,fill});
        return out;
    }
    if(resolution.type.kind==TypeKind::Class){
        const auto& info=checked_.classes.at(resolution.target);
        std::vector<std::optional<ValueId>> fields(info.fields.size());
        std::unordered_set<std::string> supplied;

        for(const auto& arg:n.args){
            const auto it=std::find_if(info.fields.begin(),info.fields.end(),
                [&](const auto& field){return field.name==*arg.name;});
            auto v=lowerer_.lower_into(*arg.value,it->type);
            fields[it->index]=v;
            supplied.insert(it->name);
        }

        for(const auto& field:info.fields){
            if(supplied.contains(field.name)||!field.default_value)continue;
            auto v=lowerer_.lower_into(*field.default_value,field.type);
            fields[field.index]=v;
        }

        auto out=builder_.fresh();
        builder_.emit(
            ClassMake{out,Type::class_type(resolution.target),std::move(fields)});
        return out;
    }
    throw std::logic_error("Unsupported constructor resolution.");
}

ValueId CallLowering::lower_function_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    const auto& sig=checked_.functions.at(resolution.target);
    auto lowered=lower_call_arguments(n.args,sig,0,resolution.target);
    const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:builder_.fresh();
    const auto shape = prepare_shaped_call(e, resolution.target, sig, lowered);
    builder_.emit(Call{out,shape.callee,lowered.args,sig.result,{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)},sig.no_normal_return});
    if (sig.result.kind != TypeKind::Never && !sig.no_normal_return)
        finish_shaped_call(e, sig, lowered, shape);
    if(sig.result.kind!=TypeKind::Never && !sig.no_normal_return)
        release_borrowed_temporaries(lowered);
    return out;
}
} // namespace quidra::lowering
