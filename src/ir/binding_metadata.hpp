#pragma once

// Common IR binding metadata: exact declaration/storage origins imported
// by lowering and consumed by analyses. These owned records contain no CFG,
// local-SSA algorithm, or object-provenance analysis result.
#include "ir/instruction_site.hpp"
#include "quidra/declaration_identity.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"
#include "semantics/storage_path.hpp"

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
struct BindingSources {
    // Runtime names are lookup keys only; proofs retain original identities.
    std::map<std::string,BindingClass,std::less<>> classes;
    std::map<std::string,FunctionBindingSource,std::less<>> functions;
    std::vector<BindingSourceIssue> issues;
};
struct BindingOriginInputs {
    std::set<std::string,std::less<>> rebind_declarations;
    std::map<std::string,FunctionBindingSource,std::less<>> functions;
};
} // namespace quidra::ir::analysis
