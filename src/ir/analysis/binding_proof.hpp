#pragma once
#include "ir/analysis/object_provenance.hpp"

namespace quidra::ir::analysis {
struct FieldWriter {
    std::string function;InstrRef at;
    std::string semantic_function;
    bool construction{},rebind{},unknown{};
};
struct FieldProof {
    ClassIdentity owner;std::size_t field{};std::string name;
    bool constructor_or_rebind_only{},available{};
    std::vector<FieldWriter> writers;
    std::vector<BindingGap> gaps;
};
struct FixedCollection {
    std::string function;InstrRef at;ValueId array{};
    Type target;bool available{},paths_available{},initialized{};
    std::vector<ObjectPath> elements; // Logical source paths, not class allocation equality.
    std::vector<ObjectFact> values; // Retains copies and closed reference target provenance.
    std::vector<BindingGap> gaps;
};
struct BoundField : FieldProof {
    std::vector<std::size_t> collections; // Exact proofs, never another class's list.
};
struct BindingProof {
    std::vector<ObjectProvenance> objects; // Module authored function order.
    std::vector<FieldProof> fields;
    std::vector<FixedCollection> collections;
    std::vector<BoundField> bound_fields;
    std::map<std::string,bool,std::less<>> constructor_allocations;
};
// Class-level writer/list metadata for the unchanged typed module. Actual
// object-sensitive no-intervening-call closure is a separate 0.19b consumer.
// No physical exclusivity, donor, lifetime or guard-deletion certificate.
BindingProof binding_proof(const Module&,const ModuleSummaries&,const BindingSources&);
// Reuses immutable observations; no CFG/dom/SSA/def-use work is repeated.
BindingProof binding_proof(const Module&,const ModuleSummaries&,const BindingSources&,
                           std::vector<ObjectProvenance> observations);
} // namespace quidra::ir::analysis
