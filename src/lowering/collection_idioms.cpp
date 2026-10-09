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
// The same get->set cache fields are updated, including missing-key reuse.
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

    const ClassFieldType* values_field = nullptr;
    const ClassFieldType* empty_slot_field = nullptr;
    const ClassFieldType* last_index_field = nullptr;
    const ClassFieldType* last_hash_field = nullptr;
    const ClassFieldType* last_slot_field = nullptr;
    const ClassFieldType* last_key_field = nullptr;
    const ClassFieldType* version_field = nullptr;
    const ClassFieldType* last_version_field = nullptr;
    for (const auto& field : class_info.fields) {
        if (field.name == "__values") values_field = &field;
        else if (field.name == "__empty_slot") empty_slot_field = &field;
        else if (field.name == "__last_index") last_index_field = &field;
        else if (field.name == "__last_hash") last_hash_field = &field;
        else if (field.name == "__last_slot") last_slot_field = &field;
        else if (field.name == "__last_key") last_key_field = &field;
        else if (field.name == "__version") version_field = &field;
        else if (field.name == "__last_version") last_version_field = &field;
    }
    if (!values_field || !empty_slot_field || !last_index_field ||
        !last_hash_field || !last_slot_field || !last_key_field ||
        !version_field || !last_version_field ||
        values_field->type.kind != TypeKind::Array ||
        last_key_field->type.kind != TypeKind::Array ||
        !values_field->type.first || !last_key_field->type.first)
        return false;

    const auto key_type = *last_key_field->type.first;
    const auto value_type = *values_field->type.first;
    if (requires_lifetime_management(key_type) ||
        requires_lifetime_management(value_type)) return false;

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
    auto key = lowerer_.lower(*call->args[0].value);
    auto hash = builder_.fresh();
    builder_.emit(Call{
        hash, hash_method->second,
        std::vector<CallArgument>{
            CallArgument{object, std::nullopt},
            CallArgument{key, std::nullopt}},
        int_type, line, column});

    auto index = builder_.fresh();
    builder_.emit(Call{
        index, find_method->second,
        std::vector<CallArgument>{
            CallArgument{object, std::nullopt},
            CallArgument{key, std::nullopt},
            CallArgument{hash, std::nullopt}},
        int_type, line, column});

    auto load_field = [&](const ClassFieldType& field) {
        auto out = builder_.fresh();
        builder_.emit(
            FieldGet{out, object, field.index, field.type});
        return out;
    };

    // Mirror Map.get's private cache updates. __find/__slot_of has already
    // stored the insertion/found slot in __empty_slot.
    builder_.emit(FieldSet{
        object, last_hash_field->index, hash, int_type});
    builder_.emit(FieldSet{
        object, last_index_field->index, index, int_type});
    auto current_slot = load_field(*empty_slot_field);
    builder_.emit(FieldSet{
        object, last_slot_field->index, current_slot, int_type});
    auto map_version = load_field(*version_field);
    builder_.emit(FieldSet{
        object, last_version_field->index, map_version, int_type});

    auto zero = builder_.const_int(0);
    auto found_test = builder_.fresh();
    builder_.emit(Binary{
        found_test, ">=", index, zero,
        int_type, bool_type, line, column});

    const auto found = builder_.label("map.match.found");
    const auto missing = builder_.label("map.match.missing");
    const auto missing_make_key = builder_.label("map.match.missing.key.make");
    const auto missing_set_key = builder_.label("map.match.missing.key.set");
    const auto value_case_label = builder_.label("map.match.value");
    const auto none_case_label = builder_.label("map.match.none");
    const auto done = builder_.label("map.match.end");
    builder_.emit(Branch{found_test, found, missing});

    builder_.enter(builder_.add_block(found));
    auto values = load_field(*values_field);
    auto payload = builder_.fresh();
    builder_.emit(ArrayGet{
        payload, values, index, value_type, line, column, true, true});
    builder_.emit(Jump{value_case_label});

    builder_.enter(builder_.add_block(missing));
    auto last_key = load_field(*last_key_field);
    auto last_key_length = builder_.fresh();
    builder_.emit(ArrayLength{last_key_length, last_key});
    auto last_key_empty = builder_.fresh();
    builder_.emit(Binary{
        last_key_empty, "==", last_key_length, zero,
        int_type, bool_type, line, column});
    builder_.emit(Branch{
        last_key_empty, missing_make_key, missing_set_key});

    builder_.enter(builder_.add_block(missing_make_key));
    auto new_last_key = builder_.fresh();
    builder_.emit(ArrayMake{
        new_last_key, std::vector<ValueId>{key}, last_key_field->type});
    builder_.emit(FieldSet{
        object, last_key_field->index, new_last_key, last_key_field->type});
    builder_.emit(Jump{none_case_label});

    builder_.enter(builder_.add_block(missing_set_key));
    builder_.emit(ArraySet{
        last_key, zero, key, key_type, line, column, true, true});
    builder_.emit(Jump{none_case_label});

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
    return true;
}

} // namespace quidra::lowering
