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
#include "lowering/lowerer.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace quidra::lowering {

namespace {

// Whether evaluating `expression` runs code beyond reading places: a call,
// a method call, `try` or an if-expression anywhere in it.
bool runs_code(const Expr& expression) {
    const auto& data = expression.data;
    if (std::holds_alternative<CallExpr>(data) || std::holds_alternative<MethodCallExpr>(data) ||
        std::holds_alternative<TryExpr>(data) || std::holds_alternative<IfExpr>(data))
        return true;
    const auto any = [](const ExprPtr& item) { return item && runs_code(*item); };
    if (const auto* node = std::get_if<MemberExpr>(&data)) return any(node->base);
    if (const auto* node = std::get_if<IndexExpr>(&data)) {
        if (any(node->base)) return true;
        for (const auto& item : node->items)
            if (any(item.index) || any(item.start) || any(item.stop) || any(item.step)) return true;
        return false;
    }
    if (const auto* node = std::get_if<UnaryExpr>(&data)) return any(node->operand);
    if (const auto* node = std::get_if<BinaryExpr>(&data)) return any(node->left) || any(node->right);
    if (const auto* node = std::get_if<StringTemplateExpr>(&data)) {
        for (const auto& item : node->expressions)
            if (any(item)) return true;
        return false;
    }
    if (const auto* node = std::get_if<ArrayExpr>(&data)) {
        for (const auto& item : node->elements)
            if (any(item)) return true;
    }
    return false;
}

} // namespace

// A fresh class value with every declared default evaluated and every
// other field uninitialized: what `Point point` and a constructor's
// receiver start from.
ValueId CallLowering::make_class_with_defaults(const std::string& class_name){
    const auto& info=checked_.classes.at(class_name);
    std::vector<std::optional<ValueId>> fields(info.fields.size());
    for(const auto& field:info.fields){
        if(!field.default_value)continue;
        fields[field.index]=lowerer_.lower_into(*field.default_value,field.type);
    }
    auto out=builder_.fresh();
    builder_.emit(ClassMake{out,Type::class_type(class_name),std::move(fields)});
    return out;
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
    for(const auto& arg:source_args){
        std::size_t target;
        if(arg.name) target=index.at(*arg.name);
        else { while(positional<filled.size()&&filled[positional]) ++positional; target=positional++; }
        filled[target]=true; const auto& param=sig.parameters[target];
        if(param.writable) lowered.args[target]=CallArgument{0,lowerer_.lower_address(*arg.value, !param.is_const)};
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
        builder_.emit(Release{value, type});
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
    if (const auto out = concurrency_.try_counter_method(e, n, receiver_type)) return *out;
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
    const bool receiver_first=runs_code(*n.receiver);
    ValueId receiver{};
    if(receiver_first) receiver=lowerer_.lower(*n.receiver);
    auto lowered=lower_call_arguments(n.args,sig,1,internal);
    if(!receiver_first) receiver=lowerer_.lower(*n.receiver);
    lowered.args[0]=CallArgument{receiver,std::nullopt};
    const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:builder_.fresh();
    builder_.emit(Call{out,internal,std::move(lowered.args),sig.result,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column),sig.no_normal_return});
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
        static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)});
    return out;
}

ValueId CallLowering::lower_enclosing_method_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    const auto& sig=checked_.functions.at(resolution.target);
    auto lowered=lower_call_arguments(n.args,sig,1,resolution.target);
    lowered.args[0]=CallArgument{load_receiver(builder_,enclosing_class_),std::nullopt};
    const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:builder_.fresh();
    builder_.emit(Call{out,resolution.target,std::move(lowered.args),sig.result,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column),sig.no_normal_return});
    if(sig.result.kind!=TypeKind::Never && !sig.no_normal_return)
        release_borrowed_temporaries(lowered);
    return out;
}

ValueId CallLowering::lower_constructor_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    if(resolution.type.kind==TypeKind::Error) {
        return lowerer_.lower(*n.args[0].value);
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
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
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
    builder_.emit(Call{out,resolution.target,std::move(lowered.args),sig.result,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column),sig.no_normal_return});
    if(sig.result.kind!=TypeKind::Never && !sig.no_normal_return)
        release_borrowed_temporaries(lowered);
    return out;
}
} // namespace quidra::lowering
