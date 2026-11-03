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
inline constexpr char file_read[] = "$std.file.Read";
inline constexpr char file_write[] = "$std.file.Write";
inline constexpr char file_append[] = "$std.file.Append";
inline constexpr char autograd_target[] = "$std.autograd.Target";
inline constexpr char time_instant[] = "$std.time.Instant";
inline constexpr char time_duration[] = "$std.time.Duration";
inline constexpr char process_result[] = "$std.process.Result";
inline constexpr char random_generator[] = "$std.random.Generator";
inline constexpr char map[] = "$std.map.Map";
inline constexpr char distinct[] = "$std.distinct.Distinct";

// The start of the names the frontend gives the instances of the generic
// standard classes: abi::generic_instance_prefix(abi::generic_class_kind, id),
// which the frontend completes with a hash of the type arguments.
// compiler_tests.cpp checks each prefix against that rule.
inline constexpr char map_instance_prefix[] = "__quidra_gc__std_map_Map_";
inline constexpr char distinct_instance_prefix[] = "__quidra_gc__std_distinct_Distinct_";

// Whether the class `name` is an instance of the built-in map or distinct type.
// Only a class of the standard library (ClassDecl::standard_library,
// passed as `standard_library`) is one: the name alone decides nothing,
// because the instance of a user's generic class can start the same way
// (`_std_map_Map<K, V>` can share the internal prefix of the built-in map).
inline bool is_map_instance(std::string_view name, bool standard_library) {
    return standard_library && name.starts_with(map_instance_prefix);
}
inline bool is_distinct_instance(std::string_view name, bool standard_library) {
    return standard_library && name.starts_with(distinct_instance_prefix);
}


// The classes whose values are runtime handles: the runtime owns the object,
// and a value is a reference to it that generated code retains and releases.
// They have no field-wise equality.
inline bool is_runtime_handle(std::string_view id) {
    return id == json_value || id == http_response || id == file_handle ||
           id == file_read || id == file_write || id == file_append ||
           id == autograd_target;
}

} // namespace quidra::standard_class
