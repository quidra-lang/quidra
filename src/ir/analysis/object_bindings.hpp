#pragma once
#include "ir/analysis/binding_proof.hpp"
#include "ir/analysis/loops.hpp"

namespace quidra::ir::analysis {
enum class ObjectBindingGap : std::uint8_t {
  MissingOrigin,
  MissingClass,
  MissingEffect,
  AmbiguousIdentity,
  MissingHolder,
  UnknownCall,
  UnboundedSharedWrite,
  BindingWrite,
  HolderWrite,
  Escaped,
  Container,
  ParallelCapture,
  LoopUnavailable,
  BirthUnavailable,
  BirthInLoop,
  TraversalUnavailable,
  FieldClosureUnavailable,
  Budget
};
// A separate must-identity domain. No payload/init/shape/ownership facts.
struct ObjectIdentityFact {
  Type type, object_type;
  std::optional<ObjectPath> object;
  std::optional<ObjectRoot> allocation;
  std::map<std::size_t, ObjectPath> references;
  std::vector<semantics::StoragePath> holders;
  bool available{}, address{}, error_return{};
  bool operator==(const ObjectIdentityFact &) const = default;
};
struct ObjectIdentityEvent {
  InstrRef at;
  std::optional<ObjectPath> object;
  ObjectBindingGap gap;
};
struct ConstructorIdentity {
  DeclarationIdentity declaration;
  Type object_type;
  std::map<std::size_t, ObjectPath>
      references; // Exact formal reference targets.
  bool available{};
};
using ConstructorIdentities =
    std::map<std::string, ConstructorIdentity, std::less<>>;
struct ObjectIdentityAnalysis {
  struct Scope {
    std::map<ValueId, ObjectIdentityFact> incoming;
    std::map<ValueId, std::vector<std::pair<std::uint32_t, ObjectIdentityFact>>>
        changes;
    std::uint32_t instruction_count{};
  };
  Cfg cfg;
  DomTree dominance;
  LoopNest loops;
  std::vector<Scope> scopes;
  std::map<ValueId, InstrRef> definitions;
  std::vector<ObjectIdentityFact> returns;
  std::vector<ObjectIdentityEvent> events;
  bool complete{};
  ObjectIdentityFact at(InstrRef, ValueId) const;
};
struct ObjectBindingRequest {
  std::size_t function{};
  std::uint32_t loop{};
  InstrRef use;
  ValueId object{};
  Type target;
};
struct BindingCallEvidence {
  std::string function, callee;
  InstrRef at;
  std::vector<ObjectPath> mapped_writes;
  bool effects_available{};
};
struct ObjectBindingFact {
  ObjectRoot allocation;
  BlockId loop_header{};
  semantics::StoragePath holder;
  ObjectPath model;
  Type model_type, target;
  ClassIdentity model_identity;
  std::size_t reference_field{};
  std::vector<std::vector<semantics::PathStep>> paths;
  std::vector<BindingCallEvidence> calls;
  // Structural membership only; initialization, shapes, gradient state,
  // uniqueness, donation and check deletion require their separate proofs.
};
struct ObjectBindingResult {
  ObjectBindingRequest request;
  std::optional<ObjectBindingFact> fact;
  std::vector<ObjectBindingGap> gaps;
};
struct ObjectBindingOptions {
  std::uint64_t maximum_operations{2'000'000};
};
struct ObjectBindingReport {
  std::vector<ObjectBindingResult> results;
  ConstructorIdentities constructors;
  std::vector<ObjectIdentityAnalysis> identities;
  std::uint64_t operations{};
  bool budget_exhausted{};
};
// Supplied contracts must be proved from actual successful constructor
// returns; declaration kind alone never authorizes a birth or foreign write.
ObjectIdentityAnalysis object_identities(const Module &, std::size_t function,
                                         const ModuleSummaries &,
                                         const BindingSources &,
                                         const ConstructorIdentities &,
                                         ObjectBindingOptions = {});
// Observational only. Inputs/indices refer to one unchanged verified module.
// Missing typed origins/effects/identity closure produce explicit gaps.
ObjectBindingReport
object_binding_closure(const Module &, const ModuleSummaries &,
                       const BindingSources &, const BindingProof &,
                       const std::vector<ObjectBindingRequest> &,
                       ObjectBindingOptions = {});
} // namespace quidra::ir::analysis
