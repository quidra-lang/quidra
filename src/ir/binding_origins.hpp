#pragma once
#include "ir/binding_metadata.hpp"
#include "quidra/ir/module.hpp"
#include "ir/canonical_encoding.hpp"

namespace quidra::ir::analysis {
struct BindingOriginSnapshot {
    FunctionBindingSource source;
    // Full location-neutral BODY payload: actual ABI/declaration/blocks and
    // semantic inline mappings; excludes the central, explicit non-body
    // annotation set in binding_origin_body_fields. Equality is a
    // stale-metadata guard only, never a purity/ownership or equivalence proof.
    std::string body_fields;
    std::vector<BindingSourceIssue> unavailable;
    std::map<std::uint64_t,ScopedBindingSource> scopes;
    bool root_source_available{true}; // False: typed carrier, no fabricated host declaration.
};
struct BindingOriginSnapshots {
    std::map<std::string,BindingOriginSnapshot,std::less<>> functions;
    std::string class_fields; // Exact typed layouts used by reflected field paths.
};
struct BindingOriginRead {
    BindingOriginInputs inputs;
    std::vector<BindingSourceIssue> unavailable;
};
std::string binding_origin_body_fields(const Function&);
BindingOriginRead read_binding_origins(const Module&);
// Explicit unchanged-body clone only: source/callee/environment equivalence is
// the transformation's obligation. Full payload, exact declaration, generic
// arguments and formals are checked; changed/unknown bodies return unavailable.
std::optional<BindingOriginSnapshot> clone_binding_origin(const Function& source,
    const Function& clone,const BindingOriginSnapshot&,std::vector<BindingSourceIssue>&);
} // namespace quidra::ir::analysis
