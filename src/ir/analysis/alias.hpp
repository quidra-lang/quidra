#pragma once

#include "ir/analysis/def_use.hpp"
#include "ir/analysis/local_ssa.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace quidra::ir::analysis {
using StorageClassId = std::uint32_t;
enum class StorageOrigin : std::uint8_t {
    Fresh, Param, BorrowedParam, BorrowedLocal, CallResult, FieldLoad,
    ElementLoad, CellLoad, ReferenceLoad, AutogradGrad, Unknown
};
struct StateSource {
    enum class Kind : std::uint8_t { Parameter, Local, Field, Element, Reference, Call } kind{};
    std::string local;
    std::optional<std::uint32_t> parameter;
    std::optional<std::size_t> member;
    // Relative local/parameter identity only. Persistent/reference target
    // identities and ModuleSummaries are not available in this foundation.
    bool identity_available{};
    bool operator==(const StateSource&) const = default;
};
struct StorageClass {
    StorageClassId id{};
    std::vector<StorageOrigin> origins;
    std::vector<InstrRef> sites;
    std::vector<StateSource> state;
    bool owned{}, borrowed{}, may_have_other_owners{}, reaches_autograd{};
    bool address_exposed{}, escapes_function{};
    bool alias_available{true}, ownership_available{true}, state_identity_available{true};
};
struct StorageValue {
    ValueId value{};
    // A tensor has possible storage classes; aggregates retain all contained
    // tensor classes. An aggregate's outer allocation is not TensorStorage.
    std::vector<StorageClassId> classes;
    bool storage_available{true};
    // Ownership provenance of this value, independent of internal class-wide
    // borrowed views. Borrowed/unknown results never qualify as owned donors.
    // Neither field proves uniqueness, last use, untrackedness or donation.
    bool owned{}, borrowed{};
};
struct StorageProjection {
    std::vector<StorageClassId> classes;
    bool storage_available{};
};
struct AliasInfo {
    std::vector<StorageValue> values; // Sorted actual SSA IDs; never max-ID sized.
    std::vector<StorageClass> classes; // Only actual union roots, compact IDs.
    std::unordered_map<std::string, std::vector<StorageClassId>> slot_classes;
    std::unordered_map<std::string, StorageProjection> slot_storage;
    std::unordered_map<std::uint32_t, StorageProjection> parameter_storage, phi_storage;
    bool complete{true};
    const StorageValue* find(ValueId) const noexcept;
    StorageProjection storage_of(const ReachingDef&) const;
    // Unknown contracts return true, including absent IDs. No-sharing is
    // certified only when both values' storage/alias information is available.
    bool may_alias(ValueId, ValueId) const noexcept;
};

// Observational, flow-insensitive may-sharing closure for the same unchanged
// typed function/analyses. No invented summaries, reference targets, new IR
// nodes, liveness or ownership-transfer verifier. Module supplies class field
// types only. Missing provenance is explicit and conservative.
AliasInfo alias_analysis(const Function&, const DefUse&, const LocalSsa&, const Slots&,
                         const Module* module = nullptr);
} // namespace quidra::ir::analysis
