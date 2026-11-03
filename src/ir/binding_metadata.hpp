#pragma once

// Common IR binding metadata: exact declaration/storage origins imported
// by lowering and consumed by analyses. These owned records contain no CFG,
// local-SSA algorithm, or object-provenance analysis result.
#include "ir/instruction_site.hpp"
#include "quidra/declaration_identity.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"
#include "semantics/storage_path.hpp"
#include "semantics/effect_summary.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Retain the established namespace and exact field layouts for compatibility.
namespace quidra::ir::analysis {
enum class BindingGap : std::uint8_t {
    MissingOrigin,MissingClass,MissingEffect,InitializationUnavailable,Ambiguous,
    DynamicIndex,ArrayAlias,Escaped,ReferenceUnavailable,UnknownCall,
    OrdinaryWriter,UnknownWriter,Unreachable,AllocationIdentityUnavailable,InlineOriginUnavailable,StaleOrigin
};
enum class BindingSourceComponent : std::uint8_t {
    Declaration,ClassLayout,ParameterMap,Receiver,ValidatedInitialization,
    LocalStoragePaths,Collections,RebindRole,Body,RebindEvents,Traversals
};
struct BindingSourceIssue {
    BindingSourceComponent component;
    BindingGap reason;
    std::string lookup; // Exact IR lookup key, never an inferred identity.
};
struct BindingSourceAvailability {
    bool parameter_map{},receiver{},validated_initialization{},local_storage_paths{},
         collections{},rebind_role{},rebind_events{},traversals{};
};
struct ClassIdentity {
    std::string package,module_path,source_name;
    std::vector<std::string> generic_arguments;
    auto operator<=>(const ClassIdentity&) const = default;
};
struct BindingField {
    std::string name;Type type;
    bool is_const{},is_private{},reference{};
};
struct BindingClass {ClassIdentity identity;std::vector<BindingField> fields;bool complete{};bool ordinary_value_layout{};};
struct CollectionOrigin {
    InstrRef at;ValueId array{},root{};Type target;
    std::vector<std::vector<semantics::PathStep>> paths;
    bool complete{};
};
struct BindingTraversalOrigin {
    enum class Kind : std::uint8_t { Collect, Each, EachPaths } kind{Kind::Each};
    InstrRef at;
    ValueId root{};
    std::optional<ValueId> output,root_address;
    Type root_type,target;
    std::vector<std::vector<semantics::PathStep>> paths;
    bool complete{},writable{};
};
struct BindingRebindEvent {
    enum class Kind : std::uint8_t { Reference, Field } kind{Kind::Reference};
    InstrRef at;
    std::string local;
    ValueId object{},address{};
    std::size_t field{};
};
struct FunctionBindingSource {
    DeclarationIdentity declaration;
    // Original declaration + exact semantic identity only. Concrete lowering
    // components have independent availability; empty is not "captured empty".
    bool complete{},rebind{},constructor_allocation_proven{};
    std::optional<std::uint32_t> receiver_ir_parameter;
    std::vector<std::optional<std::size_t>> semantic_parameter_for_ir_parameter;
    // Already validated at this entry, not declared type/initialization requirements.
    std::map<std::uint32_t,std::vector<std::vector<semantics::PathStep>>> validated_initialization;
    std::map<std::string,semantics::StoragePath,std::less<>> local_storage_paths;
    std::vector<CollectionOrigin> collections;
    BindingSourceAvailability availability;
    std::vector<BindingRebindEvent> rebind_events;
    std::vector<BindingTraversalOrigin> traversals;
};
// Original semantic coordinates are owned per inline occurrence. Their paths
// must never be interpreted in the host function's formal/local namespace.
struct InlineParameterBinding {
    std::size_t ir_parameter{};
    std::optional<std::size_t> semantic_parameter;
    std::string storage;
    Type type;
    bool writable{},borrowed{};
    ValueId value{};
    std::optional<ValueId> address;
    InstrRef at;
};
// Source and actual host coordinates remain distinct. Value returns include
// Error/fallible carriers; this record never asserts a successful payload.
struct InlineReturnBinding {
    enum class Kind : std::uint8_t { Value, Void } kind{Kind::Value};
    InstrRef original;
    ValueId original_value{};
    Type type;
    ValueId value{};
    std::optional<std::string> storage;
    std::optional<InstrRef> store;
    bool store_borrowed{},store_replace_without_release{};
    InstrRef cleanup,end,jump;
    std::string continuation;
    std::optional<InstrRef> load;
    ValueId output{};
    bool load_move_out{};
};
struct ScopedBindingSource {
    std::uint64_t scope{},parent{};
    std::string source_function,semantic_key;
    FunctionBindingSource source;
    std::optional<semantics::EffectSummary> semantic;
    InstrRef begin;
    std::vector<InlineParameterBinding> parameters;
    bool complete{};
    Type result_type; // Original concrete result ABI, including Error carriers.
    bool returns_complete{}; // Captured every Return/ReturnVoid, including empty.
    std::vector<InlineReturnBinding> returns;
};
using ScopedBindingSources = std::map<std::string,std::map<std::uint64_t,ScopedBindingSource>,std::less<>>;
struct ScopedStoragePath {
    std::string function;
    std::uint64_t scope{};
    semantics::StoragePath path;
    bool operator==(const ScopedStoragePath&) const = default;
};
struct BindingSources {
    // Runtime names are lookup keys only; proofs retain original identities.
    std::map<std::string,BindingClass,std::less<>> classes;
    std::map<std::string,FunctionBindingSource,std::less<>> functions;
    std::vector<BindingSourceIssue> issues;
    ScopedBindingSources scopes;
};
struct BindingOriginInputs {
    std::set<std::string,std::less<>> rebind_declarations;
    std::map<std::string,FunctionBindingSource,std::less<>> functions;
    ScopedBindingSources scopes;
};
} // namespace quidra::ir::analysis
