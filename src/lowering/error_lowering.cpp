// Error lowering: error(...) makes a user error (ErrorMake, schema 0, its
// text as its one detail: a constant for a string literal, else the
// string's value; its code = and its cause =), an error's message is read
// with ErrorMessage and its other members with ErrorCode, ErrorCause and
// ErrorDetail, and an error that leaves a call of user code through try, a
// fail-fast stop or a binding error case records the call (ErrorHop).

#include "lowering/error_lowering.hpp"

#include "lowering/local_scope.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/abi/error_record.hpp"
#include "quidra/member_function_names.hpp"

#include <algorithm>
#include <optional>

namespace quidra::lowering {

ValueId ErrorLowering::lower_constructor(const Expr& e, const CallExpr& n) {
    const Expr* message = nullptr;
    const Expr* cause = nullptr;
    ErrorMake make{};
    for (const auto& argument : n.args) {
        if (!argument.name || *argument.name == "message") message = argument.value.get();
        else if (*argument.name == "cause") cause = argument.value.get();
        else if (*argument.name == "code") make.code = std::get<StringExpr>(argument.value->data).value;
    }
    make.position.line = static_cast<std::uint32_t>(e.span.start.line);
    make.position.column = static_cast<std::uint32_t>(e.span.start.column);
    ValueId text = 0;
    if (const auto* literal = std::get_if<StringExpr>(&message->data)) {
        make.details.push_back(ErrorDetailOperand{std::nullopt, literal->value, false});
    } else {
        text = lowerer_.lower(*message);
        make.details.push_back(ErrorDetailOperand{text, {}, false});
    }
    ValueId cause_value = 0;
    if (cause) {
        cause_value = lowerer_.lower(*cause);
        make.cause = cause_value;
    }
    make.out = builder_.fresh();
    const auto out = make.out;
    builder_.emit(std::move(make));
    if (text != 0) lifetime_.release_temporary(*message, text);
    if (cause_value != 0) lifetime_.release_temporary(*cause, cause_value);
    return out;
}

ValueId ErrorLowering::lower_member(const Expr& e, const MemberExpr& n, const std::string& member,
                                    const Type& type) {
    const bool owned = lifetime_.expression_owns_result(*n.base);
    const auto error = lowerer_.lower(*n.base);
    const auto out = builder_.fresh();
    if (member == "code") builder_.emit(ErrorCode{out, error});
    else if (member == "message") builder_.emit(ErrorMessage{out, error});
    else if (member == "cause") builder_.emit(ErrorCause{out, error, type});
    else
        builder_.emit(ErrorDetail{
            out, error, static_cast<std::uint32_t>(abi::error_detail_name_index(member)), type});
    if (owned) release_owned(builder_,error, Type::simple(TypeKind::Error));
    (void)e;
    return out;
}

ValueId ErrorLowering::message_of(ValueId value, bool owned) {
    const auto out = builder_.fresh();
    builder_.emit(ErrorMessage{out, value});
    if (owned) release_owned(builder_,value, Type::simple(TypeKind::Error));
    return out;
}

namespace {

// The display name of a class: a generic instance's declaration name
// ("__quidra_gc_Box_<hash>" -> "Box"), and without a module qualifier.
std::string class_display_name(std::string name) {
    const std::string generic = "__quidra_";
    if (name.rfind(generic, 0) == 0) {
        const auto kind_end = name.find('_', generic.size());
        const auto hash = name.rfind('_');
        if (kind_end != std::string::npos && hash != std::string::npos && hash > kind_end)
            name = name.substr(kind_end + 1, hash - kind_end - 1);
    }
    if (const auto dot = name.rfind('.'); dot != std::string::npos) name = name.substr(dot + 1);
    return name;
}

// The display name of a function as the call writes it: its name without a
// module qualifier.
std::string written_function_name(const std::string& written) {
    const auto dot = written.rfind('.');
    return dot == std::string::npos ? written : written.substr(dot + 1);
}

} // namespace

bool ErrorLowering::user_file(const std::string& file) {
    if (const auto found = user_files_.find(file); found != user_files_.end()) return found->second;
    const auto& users = checked_.program.user_sources;
    const bool user = std::find(users.begin(), users.end(), file) != users.end();
    user_files_.emplace(file, user);
    return user;
}

std::optional<ErrorHopCall> ErrorLowering::hop_call(const Expr& expression) {
    const auto* function = builder_.function();
    if (!function || !user_file(function->source_file)) return std::nullopt;
    if (sources_.empty() && class_sources_.empty()) {
        for (const auto& declaration : checked_.program.functions)
            sources_.emplace(declaration.name, declaration.source_file);
        for (const auto& declaration : checked_.program.classes)
            class_sources_.emplace(declaration.name, declaration.source_file);
    }
    const auto position = [&](std::string name) {
        return ErrorHopCall{std::move(name), static_cast<std::uint32_t>(expression.span.start.line),
                            static_cast<std::uint32_t>(expression.span.start.column)};
    };
    const auto user_class = [&](const std::string& name) {
        const auto found = class_sources_.find(name);
        return found != class_sources_.end() && user_file(found->second);
    };
    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        const auto resolution = checked_.call_resolutions.find(&expression);
        if (resolution == checked_.call_resolutions.end()) return std::nullopt;
        switch (resolution->second.kind) {
            case CallKind::FunctionValue: {
                auto hop = position(written_function_name(call->callee) + "()");
                hop.function_value = resolution->second.target;
                return hop;
            }
            case CallKind::Function: {
                const auto found = sources_.find(resolution->second.target);
                if (found == sources_.end() || !user_file(found->second)) return std::nullopt;
                return position(written_function_name(call->callee) + "()");
            }
            case CallKind::Constructor:
                if (resolution->second.type.kind != TypeKind::Class ||
                    !user_class(resolution->second.target))
                    return std::nullopt;
                return position(class_display_name(resolution->second.target) + "()");
            default:
                return std::nullopt;
        }
    }
    if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
        const auto info = checked_.method_calls.find(&expression);
        if (info == checked_.method_calls.end()) return std::nullopt;
        const auto& internal = info->second.internal_name;
        const auto prefix = member_function_name::method_prefix;
        if (internal.rfind(prefix, 0) != 0) return std::nullopt;
        const auto rest = internal.substr(prefix.size());
        const auto dot = rest.rfind('.');
        if (dot == std::string::npos) return std::nullopt;
        const auto class_name = rest.substr(0, dot);
        if (!user_class(class_name)) return std::nullopt;
        return position(class_display_name(class_name) + "." + method->method + "()");
    }
    return std::nullopt;
}

ValueId ErrorLowering::hop(ValueId container, const Type& container_type,
                           const ErrorHopCall& call, abi::ErrorHopKind kind) {
    ValueId callee = 0;
    if (!call.function_value.empty()) {
        callee = builder_.fresh();
        if (scope_.is_source_reference(call.function_value)) {
            const auto& reference = scope_.source_reference(call.function_value);
            builder_.emit(LoadReference{callee, reference, scope_.reference_type_at(reference)});
        } else {
            const auto& local = scope_.source_local(call.function_value);
            builder_.emit(LoadLocal{callee, local, scope_.local_type_at(local)});
        }
    }
    const auto out = builder_.fresh();
    builder_.emit(ErrorHop{out, container, container_type, call.name,
                           static_cast<std::uint32_t>(kind), {call.line, call.column}, callee});
    return out;
}

} // namespace quidra::lowering
