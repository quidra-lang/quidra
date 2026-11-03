#pragma once
#include "ir/binding_origins.hpp"
#include <tuple>

namespace quidra::ir::analysis {
// Authored by the transformer at the same time it emits each replacement.
// Missing entries/removed sites are explicit; no block-offset reconstruction.
struct InlineSiteLess {
    bool operator()(InstrRef a,InstrRef b)const noexcept {
        return std::tie(a.block,a.index)<std::tie(b.block,b.index);
    }
};
struct InlineOriginMap {
    std::map<InstrRef,std::optional<InstrRef>,InlineSiteLess> instructions;
    std::map<InstrRef,InstrRef,InlineSiteLess> boundaries;
    std::map<ValueId,ValueId> values;
    std::map<std::string,std::string,std::less<>> locals;
    std::map<std::uint64_t,std::uint64_t> scopes,statements;
    std::map<std::string,std::string,std::less<>> labels;
};
struct InlineOriginTransfer {
    std::string caller,callee,caller_body,callee_body;
    InstrRef call;
    Call removed_call;
    std::uint64_t scope{},parent{};
    InstrRef begin;
    InlineOriginMap retained,copied;
    std::vector<InlineParameterBinding> parameters;
    std::vector<InlineReturnBinding> returns;
};
struct InlineOriginResult {
    std::shared_ptr<const BindingOriginSnapshots> snapshots;
    std::vector<BindingSourceIssue> unavailable;
};
// Pure preparation. The caller publishes this owner with the accepted body
// only after all expansion/growth checks; a refusal must discard both.
InlineOriginResult transfer_inline_origins(const Module&,const Function& caller,
    const Function& callee,const Function& expanded,const InlineOriginTransfer&);
bool valid_scoped_binding_source(const Module&,const Function&,const ScopedBindingSource&);
// Typed readers for consumers: root source and every complete scoped source
// are distinct contexts. Returned pointers are owned by BindingSources.
const InlineScope* inline_binding_scope(const Function&,BlockId);
const ScopedBindingSource* scoped_binding_source(const BindingSources&,
    const Function&,BlockId);
std::vector<const FunctionBindingSource*> function_binding_sources(
    const BindingSources&,std::string_view function);
} // namespace quidra::ir::analysis
