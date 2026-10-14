#pragma once

#include "ir/fact_sources.hpp"
#include "ir/binding_origins.hpp"

#include <span>

namespace quidra::ir {
enum class CloneIrProofs : std::uint8_t { Invalidate, PreserveUnchangedBody };
struct CloneReference {
    std::string clone, source;
    CloneIrProofs ir_proofs{CloneIrProofs::Invalidate};
};
struct SynthesizedFunctionReference { std::string function; };
enum class CloneContractComponent : std::uint8_t { Reference, Semantic, IrProofs, TensorFacts, Policy, BindingOrigins };
enum class CloneUnavailable : std::uint8_t {
    InvalidReference, DuplicateReference, MissingFunction, AmbiguousFunction,
    DestinationExists, MissingSourceSummary, MissingSemanticSnapshot,
    AbiMismatch, FormalMappingMismatch, MissingSourceFacts, ChangedBody,
    UncomparableBody, InitialSummaryOmission, MissingSourceOrigins
};
struct CloneContractIssue {
    CloneContractComponent component;
    CloneUnavailable reason;
    std::string function, source;
};
enum class ClonePolicyAvailability : std::uint8_t { Unavailable, NotDeclared, Present };
struct CloneContractStatus {
    std::string function, source;
    bool synthesized{}, reference_available{}, semantic_available{}, result_extents_available{}, ir_proofs_preserved{};
    // NotDeclared retains the absence of a registry contract. It is not a
    // proof that the function cannot read/write a policy.
    ClonePolicyAvailability policy_setter{ClonePolicyAvailability::Unavailable};
    ClonePolicyAvailability policy_write{ClonePolicyAvailability::Unavailable};
    bool binding_origins_available{};
};
struct CloneContractResult {
    std::shared_ptr<const analysis::ModuleSummaries> summaries;
    std::shared_ptr<const facts::FactSources> fact_sources;
    std::vector<CloneContractStatus> bindings;
    std::vector<CloneContractIssue> unavailable;
    std::shared_ptr<const analysis::BindingOriginSnapshots> binding_origins;
};

// CloneReference is an explicit obligation from a transformation that preserves
// the source instance's semantic contract under its validated caller facts.
// This adapter validates the exact formal ABI; it does not prove equivalence
// from a generated name, declaration spelling, body digest or source location.
// IR facts are invalidated by default. PreserveUnchangedBody additionally
// requires the caller to retain the source's callee/environment contracts;
// field-complete location-neutral equality checks the body and all other
// executable metadata. Normal IR summary refresh may replace retained facts.
// Only explicitly authorized synthesized/clone IDs receive conservative rows.
// An unlisted producer omission stays unavailable and is not auto-repaired.
CloneContractResult bind_clone_contracts(const Module&, std::span<const CloneReference>,
                                       std::span<const SynthesizedFunctionReference> = {});
} // namespace quidra::ir
