#pragma once

#include "ir/analysis/local_ssa.hpp"
#include "ir/analysis/def_use.hpp"
#include "ir/binding_metadata.hpp"
#include "ir/summaries.hpp"

namespace quidra::semantics {struct InterfaceSources;}
namespace quidra::ir::analysis {
BindingSources adapt_binding_sources(const Module&,const ModuleSummaries&,
                                    const semantics::InterfaceSources&,const BindingOriginInputs& = {});
struct ObjectRoot {
    enum Kind : std::uint8_t {Parameter,Allocation,ConstructorCall,Copy} kind{Parameter};
    std::string function;InstrRef at{};std::uint32_t parameter{};
    std::optional<ClassIdentity> class_identity;
    std::string constructor_declaration,constructor_function;
    std::vector<std::string> constructor_arguments;
    bool operator==(const ObjectRoot&) const = default;
};
struct ObjectPath {
    ObjectRoot root;std::vector<semantics::PathStep> steps;
    bool operator==(const ObjectPath&) const = default;
};
struct ObjectFact {
    Type type;bool available{},initialized{},array_closed{},address{},references_closed{},allocation_identity_available{};
    std::vector<ObjectPath> sources;
    std::optional<ObjectRoot> allocation;
    std::vector<semantics::StoragePath> holding_paths;
    std::vector<ObjectFact> elements; // Array contents or known class fields.
    std::vector<ObjectFact> call_arguments;
    std::optional<std::int64_t> integer;
    std::vector<BindingGap> gaps;
    std::vector<ScopedStoragePath> scoped_holding_paths;
    bool operator==(const ObjectFact&) const;
};
struct ObjectWrite {
    InstrRef at;ObjectFact object,value;std::size_t field{};
    bool reference{},unknown{};
    std::string semantic_function;
    std::uint64_t semantic_scope{};
};
struct ObjectExposure {InstrRef at;ObjectFact object;};
struct ObjectProvenance {
    struct Scope {
        std::map<ValueId,ObjectFact> incoming;
        std::map<ValueId,std::vector<std::pair<std::uint32_t,ObjectFact>>> changes;
        std::uint32_t instruction_count{};
    };
    std::vector<Scope> scopes;
    std::vector<bool> reachable;
    std::map<ValueId,InstrRef> definitions;
    DomTree dominance;
    std::vector<ObjectWrite> writes;
    std::vector<ObjectExposure> exposures;
    std::vector<ObjectFact> returns;
    ObjectFact at(InstrRef,ValueId) const;
};
// Sparse observation of exact typed storage/value paths. This never promotes
// borrowed locals, grants ownership, or equates class allocations with tensor
// storage classes. Constructors retain call-site identity even with unknown fields.
ObjectProvenance object_provenance(const Function&,const Cfg&,const DomTree&,const LocalSsa&,
                                  const Slots&,const DefUse&,const ModuleSummaries&,const BindingSources&);
} // namespace quidra::ir::analysis
