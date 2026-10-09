// Text idioms: statement sequences recognised before ordinary lowering and
// lowered as one operation: split followed by parse, split followed by a
// for loop, appending a built line, joining a string array, counting ASCII
// bytes, string builds, and matches over a UTF-8 decode or a numeric parse.
// Each returns false (having emitted nothing) when its shape does not fit.
// lower_text_sequence tries the sequences that start at one statement of a
// body, in a fixed order, for lower_block.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/language.hpp"
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

namespace quidra::lowering {

bool TextIdioms::string_build_element_supported(const Expr& expression) const {
    if (std::holds_alternative<StringExpr>(expression.data)) return true;
    if (const auto* name = local_name_expr(expression))
        return is_builtin_text_constant(name->name);
    const auto* method = std::get_if<MethodCallExpr>(&expression.data);
    if (!method || method->method != "string" || !method->args.empty() ||
        checked_.method_calls.contains(&expression))
        return false;
    const auto source = type_of(checked_, *method->receiver);
    // A bare integer part is a named value: the build releases no temporary.
    return is_fixed_integer(source) || source.kind == TypeKind::Bool ||
           (is_bare_integer(source) &&
            std::holds_alternative<NameExpr>(method->receiver->data));
}

StringBuildPart TextIdioms::lower_string_build_element(const Expr& expression) {
    if (const auto* literal = std::get_if<StringExpr>(&expression.data)) {
        const bool single_byte_ascii =
            literal->value.size() == 1 &&
            static_cast<unsigned char>(literal->value[0]) != 0 &&
            static_cast<unsigned char>(literal->value[0]) < 0x80U;
        return StringBuildPart{
            lowerer_.lower(expression), Type::simple(TypeKind::String),
            single_byte_ascii};
    }
    if (const auto* name = local_name_expr(expression);
        name && is_builtin_text_constant(name->name)) {
        const auto literal = builtin_text_constant(name->name);
        const bool single_byte_ascii =
            literal.size() == 1 &&
            static_cast<unsigned char>(literal[0]) != 0 &&
            static_cast<unsigned char>(literal[0]) < 0x80U;
        return StringBuildPart{
            lowerer_.lower(expression), Type::simple(TypeKind::String),
            single_byte_ascii};
    }
    const auto& method = std::get<MethodCallExpr>(expression.data);
    const auto source_type = type_of(checked_, *method.receiver);
    return StringBuildPart{lowerer_.lower(*method.receiver), source_type};
}

bool TextIdioms::is_signed_parse_wrapper(const std::string& target) const {
    const auto signature = checked_.functions.find(target);
    if (signature == checked_.functions.end() ||
        signature->second.result.kind != TypeKind::Int64 ||
        signature->second.parameters.size() != 1 ||
        signature->second.parameters.front().type.kind != TypeKind::String)
        return false;

    const FunctionDecl* function = nullptr;
    for (const auto& candidate : checked_.program.functions) {
        if (candidate.name == target) {
            function = &candidate;
            break;
        }
    }
    if (!function || function->parameters.size() != 1 ||
        function->body.size() != 1)
        return false;

    const auto* match =
        std::get_if<MatchStmt>(&function->body.front()->data);
    if (!match || !match->value || match->cases.size() != 2)
        return false;
    const auto* parse =
        std::get_if<MethodCallExpr>(&match->value->data);
    if (!parse || parse->method != "parse" || parse->args.size() != 1 ||
        !parse->args.front().value)
        return false;
    const auto* receiver =
        local_name_expr(*parse->receiver);
    const auto* argument =
        local_name_expr(*parse->args.front().value);
    if (!receiver || receiver->name != "int64" || !argument ||
        argument->name != function->parameters.front().name)
        return false;

    for (const auto& current : match->cases) {
        const auto type = checked_.case_types.find(&current);
        if (type == checked_.case_types.end() ||
            type->second.kind != TypeKind::Int64)
            continue;
        if (!current.binder || current.body.size() != 1)
            return false;
        const auto* returned =
            std::get_if<ReturnStmt>(&current.body.front()->data);
        if (!returned || !returned->value)
            return false;
        const auto* name =
            local_name_expr(*returned->value);
        return name && name->name == *current.binder;
    }
    return false;
}

bool TextIdioms::split_parse_call(
    const BindingStmt& binding, const std::string& fields_name,
    std::uint64_t expected_index) const {
    if (binding.reference || !binding.value) return false;
    const auto binding_type_it = checked_.expr_types.find(binding.value.get());
    if (binding_type_it == checked_.expr_types.end() ||
        binding_type_it->second.kind != TypeKind::Int64)
        return false;

    const auto matches_index =
        [&](const Expr& argument) {
            const auto* index = std::get_if<IndexExpr>(&argument.data);
            if (!index || index->items.size() != 1 ||
                index->items.front().slice ||
                !index->items.front().index)
                return false;
            const auto* base =
                local_name_expr(*index->base);
            const auto* literal =
                std::get_if<IntegerExpr>(
                    &index->items.front().index->data);
            return base && base->name == fields_name && literal &&
                   literal->fits_u64 &&
                   literal->value == expected_index;
        };

    // Direct int64.parse(fields[N]) is the canonical source form. The typed
    // int64 destination is a success-only context, so an invalid parse
    // already has the same fail-fast semantics as the wrapper form below.
    // The fused runtime helper falls back to the original path whenever it
    // cannot prove the two canonical decimal fields.
    if (const auto* parse =
            std::get_if<MethodCallExpr>(&binding.value->data)) {
        const auto* receiver =
            local_name_expr(*parse->receiver);
        if (receiver && receiver->name == "int64" &&
            parse->method == "parse" &&
            parse->type_arguments.empty() &&
            parse->args.size() == 1 &&
            !parse->args.front().writable &&
            parse->args.front().value &&
            matches_index(*parse->args.front().value)) {
            const auto raw = checked_.raw_types.find(binding.value.get());
            return raw != checked_.raw_types.end() &&
                   raw->second.kind == TypeKind::Union &&
                   case_index(raw->second, Type::simple(TypeKind::Int64)) >= 0 &&
                   case_index(raw->second, Type::simple(TypeKind::Error)) >= 0;
        }
    }

    const auto* call = std::get_if<CallExpr>(&binding.value->data);
    if (!call || call->args.size() != 1 ||
        !call->args.front().value)
        return false;
    const auto resolution =
        checked_.call_resolutions.find(binding.value.get());
    if (resolution == checked_.call_resolutions.end() ||
        resolution->second.kind != CallKind::Function ||
        !is_signed_parse_wrapper(resolution->second.target))
        return false;
    return matches_index(*call->args.front().value);
}

bool TextIdioms::lower_split_parse_pair(
    const Stmt& split_statement, const Stmt& left_statement,
    const Stmt& right_statement, bool fields_used_later) {
    if (fields_used_later) return false;
    const auto* split_binding =
        std::get_if<BindingStmt>(&split_statement.data);
    const auto* left_binding =
        std::get_if<BindingStmt>(&left_statement.data);
    const auto* right_binding =
        std::get_if<BindingStmt>(&right_statement.data);
    if (!split_binding || !left_binding || !right_binding ||
        split_binding->reference || left_binding->reference ||
        right_binding->reference || !split_binding->value)
        return false;

    const auto fields_type = checked_.binding_types.at(&split_statement);
    const auto left_type = checked_.binding_types.at(&left_statement);
    const auto right_type = checked_.binding_types.at(&right_statement);
    if (fields_type.kind != TypeKind::Array || !fields_type.first ||
        fields_type.first->kind != TypeKind::String ||
        left_type.kind != TypeKind::Int64 || right_type.kind != TypeKind::Int64)
        return false;

    const auto* split =
        std::get_if<MethodCallExpr>(&split_binding->value->data);
    if (!split || split->method != "split" || split->args.size() != 1 ||
        !split->args.front().value)
        return false;
    const auto* source_name =
        local_name_expr(*split->receiver);
    const auto* separator =
        std::get_if<StringExpr>(&split->args.front().value->data);
    if (!source_name || scope_.is_source_reference(source_name->name) ||
        checked_.field_accesses.contains(split->receiver.get()) ||
        !separator || separator->value.size() != 1)
        return false;
    const auto separator_byte =
        static_cast<unsigned char>(separator->value.front());
    if (separator_byte == 0 || separator_byte >= 0x80U)
        return false;

    const bool left_parse =
        split_parse_call(*left_binding, split_binding->name, 0);
    const bool right_parse =
        split_parse_call(*right_binding, split_binding->name, 1);
    if (!left_parse || !right_parse) return false;

    builder_.emit(locator_.locate(split_statement.span, builder_.function()));

    const auto fields_local =
        scope_.bind_source_local(builder_,split_binding->name, fields_type);
    const auto left_local =
        scope_.bind_source_local(builder_,left_binding->name, left_type);
    const auto right_local =
        scope_.bind_source_local(builder_,right_binding->name, right_type);
    builder_.emit(DeclareLocal{
        fields_local, fields_type, split_binding->name,
        static_cast<std::uint32_t>(split_statement.span.start.line),
        static_cast<std::uint32_t>(split_statement.span.start.column)});
    builder_.emit(DeclareLocal{
        left_local, left_type, left_binding->name,
        static_cast<std::uint32_t>(left_statement.span.start.line),
        static_cast<std::uint32_t>(left_statement.span.start.column)});
    builder_.emit(DeclareLocal{
        right_local, right_type, right_binding->name,
        static_cast<std::uint32_t>(right_statement.span.start.line),
        static_cast<std::uint32_t>(right_statement.span.start.column)});

    auto text = lowerer_.lower(*split->receiver);
    auto left = builder_.fresh();
    auto right = builder_.fresh();
    auto ok = builder_.fresh();
    builder_.emit(
        StringParseTwoSigned{left, right, ok, text, separator_byte});

    const auto fast = builder_.label("split.parse.fast");
    const auto slow = builder_.label("split.parse.slow");
    const auto done = builder_.label("split.parse.end");
    builder_.emit(Branch{ok, fast, slow});

    builder_.enter(builder_.add_block(fast));
    builder_.emit(
        StoreLocal{left_local, left, left_type, true});
    builder_.emit(
        StoreLocal{right_local, right, right_type, true});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(slow));
    auto split_value = lowerer_.lower(*split_binding->value);
    builder_.emit(
        StoreLocal{fields_local, split_value, fields_type});
    auto left_value =
        lowerer_.lower_into(*left_binding->value, left_type);
    builder_.emit(
        StoreLocal{left_local, left_value, left_type, true});
    auto right_value =
        lowerer_.lower_into(*right_binding->value, right_type);
    builder_.emit(
        StoreLocal{right_local, right_value, right_type, true});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(done));
    return true;
}

bool TextIdioms::lower_string_build_append_quad(
    const Stmt& array_statement, const Stmt& line_statement,
    const Stmt& append_statement, const Stmt& length_statement,
    bool fields_used_later, bool line_used_later) {
    if (fields_used_later || line_used_later) return false;
    const auto* array_binding =
        std::get_if<BindingStmt>(&array_statement.data);
    const auto* line_binding =
        std::get_if<BindingStmt>(&line_statement.data);
    const auto* append =
        std::get_if<AssignStmt>(&append_statement.data);
    const auto* length_update =
        std::get_if<AssignStmt>(&length_statement.data);
    if (!array_binding || !line_binding || !append || !length_update ||
        array_binding->reference || line_binding->reference ||
        !array_binding->value || !line_binding->value ||
        !append->compound_op.empty() || length_update->compound_op != "+")
        return false;

    const auto array_type = checked_.binding_types.at(&array_statement);
    const auto line_type = checked_.binding_types.at(&line_statement);
    if (array_type.kind != TypeKind::Array || !array_type.first ||
        array_type.first->kind != TypeKind::String ||
        line_type.kind != TypeKind::String)
        return false;

    const auto* array =
        std::get_if<ArrayExpr>(&array_binding->value->data);
    const auto* join =
        std::get_if<MethodCallExpr>(&line_binding->value->data);
    if (!array || array->elements.empty() || !join ||
        join->method != "join" || join->args.size() != 1 ||
        join->args.front().writable || !join->args.front().value)
        return false;
    const auto* join_receiver =
        local_name_expr(*join->receiver);
    const auto* separator =
        std::get_if<StringExpr>(&join->args.front().value->data);
    if (!join_receiver ||
        join_receiver->name != array_binding->name ||
        !separator || !separator->value.empty())
        return false;

    const auto* target_name =
        local_name_expr(*append->target);
    const auto* concat =
        std::get_if<BinaryExpr>(&append->value->data);
    if (!target_name || checked_.field_accesses.contains(append->target.get()) ||
        scope_.is_source_reference(target_name->name) ||
        type_of(checked_, *append->target).kind != TypeKind::String ||
        !concat || concat->op != "+")
        return false;
    const auto* concat_left =
        local_name_expr(*concat->left);
    const auto* concat_right =
        local_name_expr(*concat->right);
    if (!concat_left || concat_left->name != target_name->name ||
        !concat_right || concat_right->name != line_binding->name)
        return false;

    const auto* length_target =
        local_name_expr(*length_update->target);
    // The length is a nat: the counter adds it through the conversion to its
    // own kind, int(len(line)) or int64(len(line)).
    const auto length_type = type_of(checked_, *length_update->target);
    const Expr* length_expression = length_update->value.get();
    if (const auto* cast = std::get_if<CallExpr>(&length_expression->data);
        cast && cast->args.size() == 1 && cast->args.front().value) {
        const auto cast_resolution = checked_.call_resolutions.find(length_expression);
        if (cast_resolution != checked_.call_resolutions.end() &&
            cast_resolution->second.kind == CallKind::NumericCast)
            length_expression = cast->args.front().value.get();
    }
    const auto* length_call =
        std::get_if<CallExpr>(&length_expression->data);
    if (!length_target ||
        checked_.field_accesses.contains(length_update->target.get()) ||
        scope_.is_source_reference(length_target->name) ||
        (length_type.kind != TypeKind::Int64 && length_type.kind != TypeKind::Int) ||
        length_expression == length_update->value.get() ||
        !length_call || length_call->callee != "len" ||
        length_call->args.size() != 1 ||
        !length_call->args.front().value)
        return false;
    const auto length_resolution =
        checked_.call_resolutions.find(length_expression);
    if (length_resolution == checked_.call_resolutions.end() ||
        length_resolution->second.kind != CallKind::Builtin ||
        length_resolution->second.builtin != BuiltinCallable::Len)
        return false;
    const auto* measured =
        local_name_expr(*length_call->args.front().value);
    if (!measured || measured->name != line_binding->name)
        return false;

    for (const auto& element : array->elements) {
        if (!string_build_element_supported(*element) ||
            expression_mentions_name(*element, target_name->name))
            return false;
    }

    const auto fields_local =
        scope_.bind_source_local(builder_,array_binding->name, array_type);
    const auto line_local =
        scope_.bind_source_local(builder_,line_binding->name, line_type);
    builder_.emit(locator_.locate(array_statement.span, builder_.function()));
    builder_.emit(DeclareLocal{
        fields_local, array_type, array_binding->name,
        static_cast<std::uint32_t>(array_statement.span.start.line),
        static_cast<std::uint32_t>(array_statement.span.start.column)});
    builder_.emit(DeclareLocal{
        line_local, line_type, line_binding->name,
        static_cast<std::uint32_t>(line_statement.span.start.line),
        static_cast<std::uint32_t>(line_statement.span.start.column)});

    const auto content_local = scope_.source_local(target_name->name);
    auto content = builder_.fresh();
    builder_.emit(
        LoadLocal{content, content_local, Type::simple(TypeKind::String)});
    auto can_move = builder_.fresh();
    builder_.emit(StringCanAppendMove{can_move, content});

    const auto fast = builder_.label("string.build.append.fast");
    const auto fallback = builder_.label("string.build.append.fallback");
    const auto done = builder_.label("string.build.append.end");
    builder_.emit(Branch{can_move, fast, fallback});

    builder_.enter(builder_.add_block(fast));
    std::vector<StringBuildPart> parts;
    parts.reserve(array->elements.size());
    for (const auto& element : array->elements)
        parts.push_back(lower_string_build_element(*element));
    auto separator_value = lowerer_.lower(*join->args.front().value);
    auto moved = builder_.fresh();
    auto added_length = builder_.fresh();
    builder_.emit(StringBuildAppendMove{
        moved, added_length, content, std::move(parts), separator_value});
    lifetime_.release_temporary(*join->args.front().value, separator_value);
    builder_.emit(StoreLocal{
        content_local, moved, Type::simple(TypeKind::String), false, true});

    const auto length_local = scope_.source_local(length_target->name);
    auto old_length = builder_.fresh();
    builder_.emit(LoadLocal{old_length, length_local, length_type});
    // The appended length is an int64 count of code points, within the
    // inline range of an int word.
    const auto added = length_type.kind == TypeKind::Int64
        ? added_length : lowerer_.bare_from_int64(added_length, length_type, true);
    auto new_length = builder_.fresh();
    builder_.emit(Binary{
        new_length, "+", old_length, added,
        length_type, length_type,
        static_cast<std::uint32_t>(length_statement.span.start.line),
        static_cast<std::uint32_t>(length_statement.span.start.column)});
    builder_.emit(StoreLocal{length_local, new_length, length_type});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(fallback));
    auto fields_value = lowerer_.lower_into(*array_binding->value, array_type);
    builder_.emit(
        StoreLocal{fields_local, fields_value, array_type});
    auto line_value = lowerer_.lower_into(*line_binding->value, line_type);
    builder_.emit(
        StoreLocal{line_local, line_value, line_type});
    lowerer_.lower_statement(append_statement);
    if (!current_block_ended(builder_)) lowerer_.lower_statement(length_statement);
    if (!current_block_ended(builder_)) builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(done));
    return true;
}

bool TextIdioms::lower_string_array_join_pair(
    const Stmt& array_statement, const Stmt& join_statement,
    bool used_later) {
    if (used_later) return false;
    const auto* array_binding =
        std::get_if<BindingStmt>(&array_statement.data);
    const auto* join_binding =
        std::get_if<BindingStmt>(&join_statement.data);
    if (!array_binding || !join_binding || array_binding->reference ||
        join_binding->reference || !array_binding->value ||
        !join_binding->value)
        return false;

    const auto array_type = checked_.binding_types.at(&array_statement);
    const auto result_type = checked_.binding_types.at(&join_statement);
    if (array_type.kind != TypeKind::Array || !array_type.first ||
        array_type.first->kind != TypeKind::String ||
        result_type.kind != TypeKind::String)
        return false;

    const auto* array =
        std::get_if<ArrayExpr>(&array_binding->value->data);
    const auto* join =
        std::get_if<MethodCallExpr>(&join_binding->value->data);
    if (!array || array->elements.empty() || !join ||
        join->method != "join" || join->args.size() != 1 ||
        join->args[0].writable)
        return false;
    const auto* receiver = local_name_expr(*join->receiver);
    if (!receiver || receiver->name != array_binding->name)
        return false;
    for (const auto& element : array->elements)
        if (!string_build_element_supported(*element)) return false;

    builder_.emit(locator_.locate(array_statement.span, builder_.function()));
    std::vector<StringBuildPart> parts;
    parts.reserve(array->elements.size());
    for (const auto& element : array->elements)
        parts.push_back(lower_string_build_element(*element));

    builder_.emit(locator_.locate(join_statement.span, builder_.function()));
    auto separator = lowerer_.lower(*join->args[0].value);
    auto built = builder_.fresh();
    builder_.emit(
        StringBuild{built, std::move(parts), separator});
    lifetime_.release_temporary(*join->args[0].value, separator);

    const auto local_name =
        scope_.bind_source_local(builder_,join_binding->name, result_type);
    builder_.emit(DeclareLocal{
        local_name, result_type, join_binding->name,
        static_cast<std::uint32_t>(join_statement.span.start.line),
        static_cast<std::uint32_t>(join_statement.span.start.column)});
    builder_.emit(
        StoreLocal{local_name, built, result_type});
    return true;
}

bool TextIdioms::split_source_dead_after(
    const BindingStmt& binding,
    const std::vector<StmtPtr>& statements,
    std::size_t first_following) const {
    if (!binding.value) return false;
    const auto* split =
        std::get_if<MethodCallExpr>(&binding.value->data);
    if (!split || split->method != "split" ||
        split->args.size() != 1 || !split->args.front().value)
        return false;
    const auto* source =
        local_name_expr(*split->receiver);
    if (!source ||
        checked_.field_accesses.contains(split->receiver.get()) ||
        scope_.is_source_reference(source->name) ||
        !scope_.has_source_local(source->name))
        return false;
    for (std::size_t i = first_following; i < statements.size(); ++i)
        if (statement_mentions_name(*statements[i], source->name))
            return false;
    return true;
}

bool TextIdioms::lower_bound_string_split_for_pair(
    const Stmt& binding_statement, const Stmt& for_statement,
    bool used_later, bool move_source,
    const std::vector<const Stmt*>& prelude) {
    if (used_later) return false;
    const auto* binding =
        std::get_if<BindingStmt>(&binding_statement.data);
    const auto* loop = std::get_if<ForStmt>(&for_statement.data);
    if (!binding || !loop || binding->reference || !binding->value ||
        loop->writable || block_may_return(loop->body) ||
        borrows_.block_mutates_parameter(loop->body, loop->name))
        return false;

    const auto binding_type =
        checked_.binding_types.at(&binding_statement);
    if (binding_type.kind != TypeKind::Array ||
        !binding_type.first ||
        binding_type.first->kind != TypeKind::String)
        return false;

    const auto* iterable_name =
        local_name_expr(*loop->iterable);
    if (!iterable_name || iterable_name->name != binding->name)
        return false;

    for (const auto& statement : loop->body)
        if (statement_mentions_name(*statement, binding->name))
            return false;

    const auto* split =
        std::get_if<MethodCallExpr>(&binding->value->data);
    if (!split || split->method != "split" ||
        split->args.size() != 1 || !split->args.front().value ||
        type_of(checked_, *split->receiver).kind != TypeKind::String)
        return false;

    builder_.emit(locator_.locate(binding_statement.span, builder_.function()));

    auto text = lowerer_.lower(*split->receiver);
    auto separator = lowerer_.lower(*split->args.front().value);
    auto cursor = builder_.fresh();
    builder_.emit(
        StringSplitIterBegin{cursor, text, separator, move_source});
    lifetime_.release_temporary(*split->receiver, text);
    lifetime_.release_temporary(*split->args.front().value, separator);

    // Preserve the original split-binding execution point. Independent
    // scalar/local bindings between the split and its consuming loop are
    // lowered here, after the cursor has captured the split value but
    // before iteration begins.
    for (const auto* statement : prelude) {
        lowerer_.lower_statement(*statement);
        if (current_block_ended(builder_))
            throw std::logic_error(
                "split-loop prelude unexpectedly terminated control flow");
    }

    const auto item = Type::simple(TypeKind::String);
    const auto iter_name = scope_.bind_source_local(builder_,loop->name, item);
    const auto cond = builder_.label("split.binding.for.cond");
    const auto body_name = builder_.label("split.binding.for.body");
    const auto exhausted = builder_.label("split.binding.for.exhausted");
    const auto break_cleanup = builder_.label("split.binding.for.break");
    const auto done = builder_.label("split.binding.for.end");
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(cond));
    auto element = builder_.fresh();
    auto has_value = builder_.fresh();
    builder_.emit(
        StringSplitIterNext{element, has_value, cursor});
    builder_.emit(
        Branch{has_value, body_name, exhausted});

    builder_.enter(builder_.add_block(body_name));
    builder_.emit(
        StoreLocal{iter_name, element, item, true});
    loop_targets_.enter(cond, break_cleanup);
    lowerer_.lower_block(loop->body);
    loop_targets_.leave();
    if (!current_block_ended(builder_)) builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(exhausted));
    builder_.emit(StringSplitIterEnd{cursor});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(break_cleanup));
    builder_.emit(StringSplitIterEnd{cursor});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(done));
    return true;
}

bool TextIdioms::lower_string_split_for(const ForStmt& n) {
    if (n.writable || block_may_return(n.body) ||
        borrows_.block_mutates_parameter(n.body, n.name))
        return false;

    const auto* split =
        std::get_if<MethodCallExpr>(&n.iterable->data);
    if (!split || split->method != "split" ||
        split->args.size() != 1 || !split->args.front().value)
        return false;

    const auto receiver_type = type_of(checked_, *split->receiver);
    const auto iterable_type = type_of(checked_, *n.iterable);
    if (receiver_type.kind != TypeKind::String ||
        iterable_type.kind != TypeKind::Array ||
        !iterable_type.first ||
        iterable_type.first->kind != TypeKind::String)
        return false;

    auto text = lowerer_.lower(*split->receiver);
    auto separator = lowerer_.lower(*split->args.front().value);
    auto cursor = builder_.fresh();
    builder_.emit(
        StringSplitIterBegin{cursor, text, separator});
    lifetime_.release_temporary(*split->receiver, text);
    lifetime_.release_temporary(*split->args.front().value, separator);

    const auto item = Type::simple(TypeKind::String);
    const auto iter_name = scope_.bind_source_local(builder_,n.name, item);
    const auto cond = builder_.label("split.for.cond");
    const auto body_name = builder_.label("split.for.body");
    const auto exhausted = builder_.label("split.for.exhausted");
    const auto break_cleanup = builder_.label("split.for.break");
    const auto done = builder_.label("split.for.end");
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(cond));
    auto element = builder_.fresh();
    auto has_value = builder_.fresh();
    builder_.emit(
        StringSplitIterNext{element, has_value, cursor});
    builder_.emit(
        Branch{has_value, body_name, exhausted});

    builder_.enter(builder_.add_block(body_name));
    // The cursor owns the shared slab for the loop lifetime. Keep the loop
    // element borrowed; ordinary assignments from it still retain when a
    // value escapes the iteration.
    builder_.emit(
        StoreLocal{iter_name, element, item, true});
    loop_targets_.enter(cond, break_cleanup);
    lowerer_.lower_block(n.body);
    loop_targets_.leave();
    if (!current_block_ended(builder_)) builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(exhausted));
    builder_.emit(StringSplitIterEnd{cursor});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(break_cleanup));
    builder_.emit(StringSplitIterEnd{cursor});
    builder_.emit(Jump{done});

    builder_.enter(builder_.add_block(done));
    return true;
}

bool TextIdioms::lower_string_ascii_count_for(const ForStmt& n) {
    if (n.writable || n.body.size() != 1) return false;

    const auto* range = std::get_if<CallExpr>(&n.iterable->data);
    const auto range_resolution =
        checked_.call_resolutions.find(n.iterable.get());
    if (!range || range_resolution == checked_.call_resolutions.end() ||
        range_resolution->second.kind != CallKind::Builtin ||
        range_resolution->second.builtin != BuiltinCallable::Range)
        return false;
    for (const auto& argument : range->args)
        if (argument.name || argument.writable || !argument.value)
            return false;

    const auto literal_is = [](const Expr* expression,
                               std::uint64_t expected) {
        if (!expression) return false;
        const auto* value =
            std::get_if<IntegerExpr>(&expression->data);
        return value && value->fits_u64 && value->value == expected;
    };

    const Expr* end = nullptr;
    if (range->args.size() == 1) {
        end = range->args[0].value.get();
    } else if (range->args.size() == 2) {
        if (!literal_is(range->args[0].value.get(), 0)) return false;
        end = range->args[1].value.get();
    } else if (range->args.size() == 3) {
        if (!literal_is(range->args[0].value.get(), 0) ||
            !literal_is(range->args[2].value.get(), 1))
            return false;
        end = range->args[1].value.get();
    } else {
        return false;
    }
    if (!end || !is_integer_family_type(type_of(checked_, *end))) return false;

    const auto* branch = std::get_if<IfStmt>(&n.body.front()->data);
    if (!branch || !branch->else_body.empty() ||
        branch->then_body.size() != 1)
        return false;

    const auto* comparison =
        std::get_if<BinaryExpr>(&branch->condition->data);
    if (!comparison ||
        (comparison->op != "==" && comparison->op != "!="))
        return false;

    const IndexExpr* indexed = nullptr;
    const StringExpr* literal = nullptr;
    if (const auto* left =
            std::get_if<IndexExpr>(&comparison->left->data);
        left &&
        std::holds_alternative<StringExpr>(comparison->right->data)) {
        indexed = left;
        literal = &std::get<StringExpr>(comparison->right->data);
    } else if (const auto* right =
                   std::get_if<IndexExpr>(&comparison->right->data);
               right &&
               std::holds_alternative<StringExpr>(
                   comparison->left->data)) {
        indexed = right;
        literal = &std::get<StringExpr>(comparison->left->data);
    }
    if (!indexed || !literal || indexed->items.size() != 1 ||
        indexed->items.front().slice ||
        !indexed->items.front().index ||
        type_of(checked_, *indexed->base).kind != TypeKind::String ||
        literal->value.size() != 1)
        return false;

    const auto byte =
        static_cast<unsigned char>(literal->value.front());
    if (byte == 0 || byte >= 0x80U) return false;

    const auto* text_name =
        local_name_expr(*indexed->base);
    const auto* index_name =
        local_name_expr(*indexed->items.front().index);
    if (!text_name || !index_name ||
        index_name->name != n.name ||
        checked_.field_accesses.contains(indexed->base.get()) ||
        scope_.is_source_reference(text_name->name))
        return false;

    const auto* assignment =
        std::get_if<AssignStmt>(&branch->then_body.front()->data);
    if (!assignment || assignment->compound_op != "+" ||
        type_of(checked_, *assignment->target).kind != TypeKind::Int64)
        return false;
    const auto* target =
        local_name_expr(*assignment->target);
    const auto* increment =
        std::get_if<IntegerExpr>(&assignment->value->data);
    if (!target || target->name == n.name ||
        checked_.field_accesses.contains(assignment->target.get()) ||
        scope_.is_source_reference(target->name) ||
        !increment || !increment->fits_u64 ||
        increment->value != 1)
        return false;

    // range() evaluates its bound once before entering the loop. Preserve
    // that ordering; loading an immutable string local and initialized int
    // local has no observable side effect.
    auto count = lowerer_.lower_int64(*end);
    auto text = lowerer_.lower(*indexed->base);
    const auto target_name = scope_.source_local(target->name);
    auto initial = builder_.fresh();
    builder_.emit(
        LoadLocal{initial, target_name, Type::simple(TypeKind::Int64)});
    facts_.array_bounds.invalidate_length_relation(target->name);

    auto out = builder_.fresh();
    // The loop's element index failure reports the index operand, as the
    // StringIndexAsciiCompare it replaces does.
    const auto& index_at = indexed->items.front().index->span.start;
    builder_.emit(StringAsciiCountPrefix{
        out, text, count, initial, byte, comparison->op == "!=",
        static_cast<std::uint32_t>(index_at.line),
        static_cast<std::uint32_t>(index_at.column),
        static_cast<std::uint32_t>(
            branch->then_body.front()->span.start.line),
        static_cast<std::uint32_t>(
            branch->then_body.front()->span.start.column)});
    builder_.emit(
        StoreLocal{target_name, out, Type::simple(TypeKind::Int64)});
    return true;
}

bool TextIdioms::lower_utf8_array_match(const MatchStmt& match) {
    const auto* conversion =
        std::get_if<MethodCallExpr>(&match.value->data);
    if (!conversion || conversion->method != "from_utf8" ||
        conversion->args.size() != 1 ||
        !conversion->args.front().value)
        return false;
    const auto* receiver =
        local_name_expr(*conversion->receiver);
    if (!receiver || receiver->name != "string")
        return false;

    const auto* packed =
        std::get_if<CallExpr>(&conversion->args.front().value->data);
    if (!packed || packed->callee != "bin" ||
        packed->args.size() != 1 || !packed->args.front().value)
        return false;
    const auto packed_resolution =
        checked_.call_resolutions.find(conversion->args.front().value.get());
    if (packed_resolution == checked_.call_resolutions.end() ||
        packed_resolution->second.type.kind != TypeKind::Bin ||
        (packed_resolution->second.kind != CallKind::NumericCast &&
         packed_resolution->second.kind != CallKind::Constructor))
        return false;

    const auto& source_expression = *packed->args.front().value;
    const auto source_type = type_of(checked_, source_expression);
    if (source_type.kind != TypeKind::Array || !source_type.first ||
        source_type.first->kind != TypeKind::Nat8 ||
        !facts_.array_initialization.fully_initialized(source_expression))
        return false;
    const auto* source_name =
        local_name_expr(source_expression);
    if (!source_name || scope_.is_source_reference(source_name->name) ||
        checked_.field_accesses.contains(&source_expression))
        return false;

    const auto result_type = type_of(checked_, *match.value);
    if (result_type.kind != TypeKind::Union || match.cases.size() != 2)
        return false;
    const MatchCase* value_case = nullptr;
    const MatchCase* error_case = nullptr;
    for (const auto& current : match.cases) {
        const auto current_type = checked_.case_types.at(&current);
        if (current_type.kind == TypeKind::String) value_case = &current;
        else if (current_type.kind == TypeKind::Error) error_case = &current;
        else return false;
    }
    if (!value_case || !error_case) return false;

    auto array = lowerer_.lower(source_expression);
    auto text = builder_.fresh();
    auto ok = builder_.fresh();
    auto error_value = builder_.fresh();
    builder_.emit(
        StringFromUtf8ArrayDirect{text, ok, error_value, array});

    const auto value_label = builder_.label("utf8.array.value");
    const auto error_label = builder_.label("utf8.array.error");
    const auto done = builder_.label("utf8.array.end");
    builder_.emit(Branch{ok, value_label, error_label});

    const auto before = scope_.snapshot_local_names();
    const auto before_full = facts_.array_initialization.full_arrays();
    std::optional<std::unordered_set<std::string>> joined_full;

    auto lower_case = [&](const MatchCase& current,
                          const std::string& case_label,
                          ValueId payload, const Type& payload_type,
                          bool release_unbound) {
        builder_.enter(builder_.add_block(case_label));
        scope_.restore(before);
        facts_.array_initialization.set_full_arrays(before_full);
        if (current.binder) {
            const auto binder_name =
                scope_.bind_source_local(builder_,*current.binder, payload_type);
            builder_.emit(
                StoreLocal{binder_name, payload, payload_type, false});
        } else if (release_unbound) {
            builder_.emit(
                Release{payload, payload_type});
        }
        for (const auto& statement : current.body) {
            lowerer_.lower_statement(*statement);
            if (current_block_ended(builder_)) break;
        }
        if (!current_block_ended(builder_)) {
            if (joined_full)
                *joined_full = intersect_full_arrays(
                    *joined_full, facts_.array_initialization.full_arrays());
            else
                joined_full = facts_.array_initialization.full_arrays();
            builder_.emit(Jump{done});
        }
    };

    lower_case(
        *value_case, value_label, text,
        Type::simple(TypeKind::String), true);
    lower_case(
        *error_case, error_label, error_value,
        Type::simple(TypeKind::Error), false);

    builder_.enter(builder_.add_block(done));
    scope_.restore(before);
    if (joined_full) facts_.array_initialization.set_full_arrays(std::move(*joined_full));
    else facts_.array_initialization.clear_full_arrays();
    return true;
}

// Numeric parse followed immediately by match does not need the
// heap-backed T | error container on the successful path. Parse into scalar
// storage, branch on success, and expose the same static error value only
// on the failure branch.
bool TextIdioms::lower_numeric_parse_match(const MatchStmt& match) {
    const auto* call = std::get_if<MethodCallExpr>(&match.value->data);
    if (!call || call->method != "parse" || call->args.size() != 1)
        return false;
    const auto* receiver = local_name_expr(*call->receiver);
    if (!receiver) return false;
    const auto target = builtin_scalar_type(receiver->name);
    if (!target || !(is_fixed_integer(*target) ||
                     target->kind == TypeKind::Real32 ||
                     target->kind == TypeKind::Real64))
        return false;

    const auto result_type = type_of(checked_, *match.value);
    if (result_type.kind != TypeKind::Union || match.cases.size() != 2)
        return false;

    const MatchCase* value_case = nullptr;
    const MatchCase* error_case = nullptr;
    for (const auto& current : match.cases) {
        const auto current_type = checked_.case_types.at(&current);
        if (current_type == *target) value_case = &current;
        else if (current_type.kind == TypeKind::Error) error_case = &current;
        else return false;
    }
    if (!value_case || !error_case) return false;

    auto text = lowerer_.lower(*call->args[0].value);
    auto parsed = builder_.fresh();
    auto ok = builder_.fresh();
    auto error_value = builder_.fresh();
    builder_.emit(ParseNumberDirect{
        parsed, ok, error_value, text, *target});
    lifetime_.release_temporary(*call->args[0].value, text);

    const auto value_label = builder_.label("parse.match.value");
    const auto error_label = builder_.label("parse.match.error");
    const auto done = builder_.label("parse.match.end");
    builder_.emit(Branch{ok, value_label, error_label});

    const auto before = scope_.snapshot_local_names();
    const auto before_full = facts_.array_initialization.full_arrays();
    std::optional<std::unordered_set<std::string>> joined_full;

    auto lower_case = [&](const MatchCase& current,
                          const std::string& case_label,
                          ValueId payload) {
        builder_.enter(builder_.add_block(case_label));
        scope_.restore(before);
        facts_.array_initialization.set_full_arrays(before_full);
        const auto current_type = checked_.case_types.at(&current);
        if (current.binder) {
            const auto binder_name =
                scope_.bind_source_local(builder_,*current.binder, current_type);
            builder_.emit(StoreLocal{
                binder_name, payload, current_type, false});
        }
        for (const auto& statement : current.body) {
            lowerer_.lower_statement(*statement);
            if (current_block_ended(builder_)) break;
        }
        if (!current_block_ended(builder_)) {
            if (joined_full)
                *joined_full = intersect_full_arrays(
                    *joined_full, facts_.array_initialization.full_arrays());
            else
                joined_full = facts_.array_initialization.full_arrays();
            builder_.emit(Jump{done});
        }
    };

    lower_case(*value_case, value_label, parsed);
    lower_case(*error_case, error_label, error_value);

    builder_.enter(builder_.add_block(done));
    scope_.restore(before);
    if (joined_full) facts_.array_initialization.set_full_arrays(std::move(*joined_full));
    else facts_.array_initialization.clear_full_arrays();
    return true;
}

std::optional<std::size_t> TextIdioms::lower_text_sequence(
    const std::vector<StmtPtr>& statements, std::size_t i) {
    bool split_sequence_lowered = false;
    if (const auto* split_sequence_binding =
            std::get_if<BindingStmt>(&statements[i]->data);
        split_sequence_binding && !split_sequence_binding->reference) {
        const bool move_split_source = split_source_dead_after(
            *split_sequence_binding, statements, i + 1);
        std::vector<const Stmt*> prelude;
        for (std::size_t k = i + 1; k < statements.size(); ++k) {
            if (const auto* loop =
                    std::get_if<ForStmt>(&statements[k]->data)) {
                const auto* iterable_name =
                    local_name_expr(*loop->iterable);
                if (!iterable_name ||
                    iterable_name->name != split_sequence_binding->name)
                    break;
                bool split_used_later = false;
                for (std::size_t j = k + 1;
                     j < statements.size() && !split_used_later; ++j) {
                    split_used_later = statement_mentions_name(
                        *statements[j], split_sequence_binding->name);
                }
                if (lower_bound_string_split_for_pair(
                        *statements[i], *statements[k],
                        split_used_later, move_split_source, prelude)) {
                    i = k;
                    split_sequence_lowered = true;
                }
                break;
                break;
            }

            // Only cross ordinary local bindings. Cursor creation stays
            // at the original split statement, so their execution order
            // and side effects remain unchanged.
            const auto* middle =
                std::get_if<BindingStmt>(&statements[k]->data);
            if (!middle || middle->reference ||
                statement_mentions_name(
                    *statements[k], split_sequence_binding->name))
                break;
            prelude.push_back(statements[k].get());
        }
    }
    if (split_sequence_lowered) return i;
    if (i + 1 < statements.size()) {
        const auto* split_sequence_binding =
            std::get_if<BindingStmt>(&statements[i]->data);
        bool split_used_later = false;
        if (split_sequence_binding) {
            for (std::size_t j = i + 2;
                 j < statements.size() && !split_used_later; ++j) {
                split_used_later = statement_mentions_name(
                    *statements[j], split_sequence_binding->name);
            }
        }
        if (split_sequence_binding &&
            lower_bound_string_split_for_pair(
                *statements[i], *statements[i + 1],
                split_used_later,
                split_source_dead_after(
                    *split_sequence_binding, statements, i + 1))) {
            return i + 1;
        }
    }
    if (i + 3 < statements.size()) {
        const auto* fields_binding =
            std::get_if<BindingStmt>(&statements[i]->data);
        const auto* line_binding =
            std::get_if<BindingStmt>(&statements[i + 1]->data);
        bool fields_used_later = false;
        bool line_used_later = false;
        if (fields_binding) {
            for (std::size_t j = i + 2;
                 j < statements.size() && !fields_used_later; ++j) {
                fields_used_later = statement_mentions_name(
                    *statements[j], fields_binding->name);
            }
        }
        if (line_binding) {
            for (std::size_t j = i + 4;
                 j < statements.size() && !line_used_later; ++j) {
                line_used_later = statement_mentions_name(
                    *statements[j], line_binding->name);
            }
        }
        if (fields_binding && line_binding &&
            lower_string_build_append_quad(
                *statements[i], *statements[i + 1],
                *statements[i + 2], *statements[i + 3],
                fields_used_later, line_used_later)) {
            return i + 3;
        }
    }
    if (i + 2 < statements.size()) {
        const auto* split_binding =
            std::get_if<BindingStmt>(&statements[i]->data);
        bool fields_used_later = false;
        if (split_binding) {
            for (std::size_t j = i + 3;
                 j < statements.size() && !fields_used_later; ++j) {
                fields_used_later = statement_mentions_name(
                    *statements[j], split_binding->name);
            }
        }
        if (split_binding &&
            lower_split_parse_pair(
                *statements[i], *statements[i + 1],
                *statements[i + 2], fields_used_later)) {
            return i + 2;
        }
    }
    if (i + 1 < statements.size()) {
        const auto* binding =
            std::get_if<BindingStmt>(&statements[i]->data);
        bool used_later = false;
        if (binding) {
            for (std::size_t j = i + 2;
                 j < statements.size() && !used_later; ++j) {
                used_later = statement_mentions_name(
                    *statements[j], binding->name);
            }
        }
        if (binding &&
            lower_string_array_join_pair(
                *statements[i], *statements[i + 1], used_later)) {
            return i + 1;
        }
    }
    return std::nullopt;
}
} // namespace quidra::lowering
