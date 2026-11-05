// Collection idioms: a match over a standard Map.get(key) result, lowered
// without materializing the optional value.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/standard_classes.hpp"

namespace quidra::lowering {

// A successful standard Map<K,V>.get() normally returns V | none through
// the general heap-backed union ABI. In a match the union is immediately
// unpacked, so for direct scalar K/V types call the compiler-owned hash/find
// helpers directly and branch to the cases without materializing that union.
// Like Map.get, this lookup only reads the receiver.
bool CollectionIdioms::lower_standard_map_get_match(const MatchStmt& match) {
    const auto* call = std::get_if<MethodCallExpr>(&match.value->data);
    if (!call || call->method != "get" || call->args.size() != 1) return false;
    const auto* receiver_name = std::get_if<NameExpr>(&call->receiver->data);
    if (!receiver_name || scope_.is_source_reference(receiver_name->name) ||
        checked_.field_accesses.contains(call->receiver.get())) return false;

    const auto map_type = type_of(checked_, *call->receiver);
    if (map_type.kind != TypeKind::Class) return false;
    const auto class_it = checked_.classes.find(map_type.class_name);
    if (class_it == checked_.classes.end()) return false;
    const auto& class_info = class_it->second;
    if (!standard_class::is_map_instance(map_type.class_name, class_info.standard_library))
        return false;

    const ClassFieldType* keys_field = nullptr;
    const ClassFieldType* values_field = nullptr;
    for (const auto& field : class_info.fields) {
        if (field.name == "__keys") keys_field = &field;
        else if (field.name == "__values") values_field = &field;
    }
    if (!keys_field || !values_field ||
        keys_field->type.kind != TypeKind::Array ||
        values_field->type.kind != TypeKind::Array ||
        !keys_field->type.first || !values_field->type.first)
        return false;

    const auto key_type = *keys_field->type.first;
    const auto value_type = *values_field->type.first;
    if (owns_managed(key_type) ||
        owns_managed(value_type)) return false;

    const auto union_type = type_of(checked_, *match.value);
    if (union_type.kind != TypeKind::Union || match.cases.size() != 2)
        return false;
    const MatchCase* value_case = nullptr;
    const MatchCase* none_case = nullptr;
    for (const auto& current : match.cases) {
        const auto current_type = checked_.case_types.at(&current);
        if (current_type == value_type) value_case = &current;
        else if (current_type.kind == TypeKind::None) none_case = &current;
        else return false;
    }
    if (!value_case || !none_case) return false;

    const auto hash_method = class_info.methods.find("__hash");
    const auto find_method = class_info.methods.find("__find");
    if (hash_method == class_info.methods.end() ||
        find_method == class_info.methods.end() ||
        !checked_.functions.contains(hash_method->second) ||
        !checked_.functions.contains(find_method->second))
        return false;

    const auto int_type = Type::simple(TypeKind::Int64);
    const auto bool_type = Type::simple(TypeKind::Bool);
    const auto line =
        static_cast<std::uint32_t>(match.value->span.start.line);
    const auto column =
        static_cast<std::uint32_t>(match.value->span.start.column);

    auto object = lowerer_.lower(*call->receiver);
    auto key = lowerer_.lower_argument(*call->args[0].value);
    auto hash = builder_.fresh();
    builder_.emit(Call{
        hash, hash_method->second,
        std::vector<CallArgument>{
            CallArgument{object, std::nullopt},
            CallArgument{key, std::nullopt}},
        int_type, {line, column}});

    auto index = builder_.fresh();
    builder_.emit(Call{
        index, find_method->second,
        std::vector<CallArgument>{
            CallArgument{object, std::nullopt},
            CallArgument{key, std::nullopt},
            CallArgument{hash, std::nullopt}},
        int_type, {line, column}});

    auto load_field = [&](const ClassFieldType& field) {
        auto out = builder_.fresh();
        builder_.emit(
            FieldGet{out, object, field.index, field.type});
        return out;
    };

    auto zero = builder_.const_int(0);
    auto found_test = builder_.fresh();
    builder_.emit(Binary{
        found_test, ">=", index, zero,
        int_type, bool_type, line, column});

    const auto found = builder_.label("map.match.found");
    const auto value_case_label = builder_.label("map.match.value");
    const auto none_case_label = builder_.label("map.match.none");
    const auto done = builder_.label("map.match.end");
    builder_.emit(Branch{found_test, found, none_case_label});

    builder_.enter(builder_.add_block(found));
    auto values = load_field(*values_field);
    auto payload = builder_.fresh();
    builder_.emit(ArrayGet{
        payload, values, index, value_type, {line, column}, true, true});
    builder_.emit(Jump{value_case_label});

    const auto before = scope_.snapshot_local_names();
    const auto before_full = facts_.array_initialization.full_arrays();
    std::optional<std::unordered_set<std::string>> joined_full;

    auto lower_case = [&](const MatchCase& current,
                          const std::string& case_label,
                          std::optional<ValueId> case_payload) {
        builder_.enter(builder_.add_block(case_label));
        scope_.restore(before);
        facts_.array_initialization.set_full_arrays(before_full);
        const auto current_type = checked_.case_types.at(&current);
        if (current.binder && case_payload &&
            current_type.kind != TypeKind::Void &&
            current_type.kind != TypeKind::None) {
            const auto binder_name =
                scope_.bind_source_local(builder_,*current.binder, current_type);
            builder_.emit(StoreLocal{
                binder_name, *case_payload, current_type, true});
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

    lower_case(*value_case, value_case_label, payload);
    lower_case(*none_case, none_case_label, std::nullopt);

    builder_.enter(builder_.add_block(done));
    scope_.restore(before);
    if (joined_full) facts_.array_initialization.set_full_arrays(std::move(*joined_full));
    else facts_.array_initialization.clear_full_arrays();
    lowerer_.release_argument(*call->args[0].value, key);
    return true;
}

} // namespace quidra::lowering
