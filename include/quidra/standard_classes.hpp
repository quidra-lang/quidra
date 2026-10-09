#pragma once

// The classes of the standard library that the compiler knows by name: the
// internal id of each, under which the frontend registers it, the checker
// types it and the lowering recognizes it; the start of the names the
// frontend gives the instances of the generic ones; and the named sets of
// these classes that call sites ask about.
//
// Owns: the ids and the instance prefixes. A class's members and methods
// stay where the frontend declares them. Whether a class belongs to the
// standard library at all is its origin (ClassDecl::standard_library), never
// its name.

#include <string_view>

namespace quidra::standard_class {

inline constexpr char json_value[] = "$std.json.Value";
inline constexpr char http_response[] = "$std.http.Response";
inline constexpr char file_handle[] = "$std.file.Handle";
inline constexpr char atomic_counter[] = "$std.atomic.Counter";
inline constexpr char autograd_target[] = "$std.autograd.Target";
inline constexpr char time_instant[] = "$std.time.Instant";
inline constexpr char time_duration[] = "$std.time.Duration";
inline constexpr char process_result[] = "$std.process.Result";
inline constexpr char random_generator[] = "$std.random.Generator";
inline constexpr char map[] = "$std.map.Map";
inline constexpr char set[] = "$std.set.Set";
inline constexpr char ref_cell[] = "$std.ref.Cell";

// The start of the names the frontend gives the instances of the generic
// standard classes: abi::generic_instance_prefix(abi::generic_class_kind, id),
// which the frontend completes with a hash of the type arguments.
// compiler_tests.cpp checks each prefix against that rule.
inline constexpr char map_instance_prefix[] = "__quidra_gc__std_map_Map_";
inline constexpr char set_instance_prefix[] = "__quidra_gc__std_set_Set_";
inline constexpr char ref_cell_instance_prefix[] = "__quidra_gc__std_ref_Cell_";

// Whether the class `name` is an instance of map.Map, of set.Set or of
// ref.Cell. Only a class of the standard library (ClassDecl::standard_library,
// passed as `standard_library`) is one: the name alone decides nothing,
// because the instance of a user's generic class can start the same way
// (`_std_map_Map<K, V>` is mangled like map.Map).
inline bool is_map_instance(std::string_view name, bool standard_library) {
    return standard_library && name.starts_with(map_instance_prefix);
}
inline bool is_set_instance(std::string_view name, bool standard_library) {
    return standard_library && name.starts_with(set_instance_prefix);
}
inline bool is_ref_cell_instance(std::string_view name, bool standard_library) {
    return standard_library && name.starts_with(ref_cell_instance_prefix);
}

// The classes whose values are runtime handles: the runtime owns the object,
// and a value is a reference to it that generated code retains and releases.
// They have no field-wise equality.
inline bool is_runtime_handle(std::string_view id) {
    return id == json_value || id == http_response || id == file_handle ||
           id == atomic_counter || id == autograd_target;
}

} // namespace quidra::standard_class
