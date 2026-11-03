#include "ir/analysis/summary_facts.hpp"
#include "ir/analysis/inline_scopes.hpp"
#include "ir/analysis/analysis_cache.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace quidra::ir::analysis {
namespace {
using namespace effect_bit;
struct Body {
    IrSummaryFacts facts{ResultAlias::Unknown, {}, false, false, true, 0, true};
    std::vector<std::string> callees;
};
bool may_read_tensor(const Type& type) {
    if (type.kind == TypeKind::Tensor || type.kind == TypeKind::Class ||
        type.kind == TypeKind::Auto || type.kind == TypeKind::Invalid) return true;
    if (type.kind == TypeKind::Function) return false;
    if (type.first && may_read_tensor(*type.first)) return true;
    return std::any_of(type.parameters.begin(), type.parameters.end(), may_read_tensor) ||
        std::any_of(type.cases.begin(), type.cases.end(), may_read_tensor);
}
void opaque(IrSummaryFacts& facts) {
    facts.effects |= Opaque;
    facts.effects_complete = false;
    facts.descriptor_complete = false;
}
Body body(const Function& function) {
    Body result;
    if (function.external_symbol || function.blocks.empty()) opaque(result.facts);
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            constexpr auto domain = InstructionTraits<T>::domain;
            auto& facts = result.facts;
            if constexpr (std::is_same_v<T, InlineScopeCleanup>) {
                if(inline_cleanup_unknown(function,inline_scope(function,node.scope)))opaque(facts);
            } else if constexpr (std::is_same_v<T, InlineScopeBegin>) {
                const auto& scope=inline_scope(function,node.scope);
                if(scope.node_cleanup || std::any_of(scope.slots.begin(),scope.slots.end(),[](const auto& slot){
                    return slot.kind==InlineSlotKind::PinnedReference || (slot.kind==InlineSlotKind::OwnedLocal &&
                        (owns_managed(slot.type) || slot.type.kind==TypeKind::Real || is_bare_integer(slot.type)));}))facts.effects |= Alloc | MayFail;
            } else if constexpr (std::is_same_v<T, Call>) result.callees.push_back(node.callee);
            else if constexpr (std::is_same_v<T, FunctionRef>) {}
            else if constexpr (std::is_same_v<T, IndirectCall>) opaque(facts);
            else if constexpr (domain == Domain::debug) {}
            else if constexpr (domain == Domain::control_flow) {
                if constexpr (std::is_same_v<T, FailError>) facts.effects |= MayFail;
                if constexpr (std::is_same_v<T, Exit> || std::is_same_v<T, InlineExit>) { facts.effects |= ExternalIO; opaque(facts); }
            } else if constexpr (domain == Domain::constant) {
                if constexpr (!std::is_same_v<T, ConstantBool>) facts.effects |= Alloc | MayFail;
            } else if constexpr (domain == Domain::numeric) {
                facts.effects |= Alloc | MayFail;
                if constexpr (std::is_same_v<T, Unary>) {
                    if (node.type.kind == TypeKind::Tensor) facts.effects |= DeviceWork | MayFailDeferred | ReadsPolicy;
                }
                if constexpr (std::is_same_v<T, Binary>) {
                    if (node.operand_type.kind == TypeKind::Tensor || node.result_type.kind == TypeKind::Tensor)
                        facts.effects |= DeviceWork | MayFailDeferred | ReadsPolicy;
                }
            } else if constexpr (domain == Domain::check) facts.effects |= MayFail;
            else if constexpr (domain == Domain::tensor) {
                if constexpr (std::is_same_v<T, TensorDevice> || std::is_same_v<T, TensorIsContiguous>) {}
                else if constexpr (std::is_same_v<T, TensorShape>) facts.effects |= Alloc | MayFail;
                else if constexpr (std::is_same_v<T, GpuSync>) facts.effects |= HostSync | MayFailDeferred;
                else {
                    facts.effects |= DeviceWork | Alloc | MayFail | MayFailDeferred | ReadsPolicy;
                    if constexpr (std::is_same_v<T, TensorTransfer> || std::is_same_v<T, TensorItem> ||
                                  std::is_same_v<T, TensorBoolReduce>) facts.effects |= HostSync;
                    if constexpr (std::is_same_v<T, TensorSet>) facts.descriptor_effects = true;
                }
            } else if constexpr (domain == Domain::autograd) {
                facts.descriptor_effects = true;
                facts.effects |= Alloc | MayFail;
                if constexpr (std::is_same_v<T, TensorBackward>)
                    facts.effects |= DeviceWork | HostSync | MayFailDeferred | ReadsPolicy;
            } else if constexpr (domain == Domain::memory) {
                if constexpr (std::is_same_v<T, LoadAddress> || std::is_same_v<T, LoadReference>) facts.effects |= MayFail;
                if constexpr (std::is_same_v<T, AddressElement>) facts.effects |= MayFail;
                if constexpr (std::is_same_v<T, StoreLocal>) {
                    if (!node.borrowed && !node.replace_without_release && unknown_cleanup_type(node.type)) opaque(facts);
                }
                if constexpr (std::is_same_v<T, StoreAddress> || std::is_same_v<T, StoreReference>) {
                    if (unknown_cleanup_type(node.type)) opaque(facts);
                }
            } else if constexpr (domain == Domain::lifetime) {
                if constexpr (std::is_same_v<T, Clone>) facts.effects |= Alloc | MayFail;
                if constexpr (std::is_same_v<T, Release>) { if (unknown_cleanup_type(node.type)) opaque(facts); }
                if constexpr (std::is_same_v<T, Unpin>) { if(unknown_cleanup_type(cleanup_operand_type(function,node.value)))opaque(facts); }
            } else if constexpr (domain == Domain::aggregate) {
                facts.effects |= Alloc | MayFail;
                if constexpr (std::is_same_v<T, FieldSet>) { if (!node.replace_without_release && unknown_cleanup_type(node.field_type)) opaque(facts); }
                if constexpr (std::is_same_v<T, ArraySet>) { if (unknown_cleanup_type(node.element_type)) opaque(facts); }
                if constexpr (std::is_same_v<T, BindFieldReference>) opaque(facts);
            } else if constexpr (domain == Domain::conversion || domain == Domain::bin ||
                                 domain == Domain::text || domain == Domain::json || domain == Domain::error) {
                facts.effects |= Alloc | MayFail;
                if constexpr (std::is_same_v<T, ToString>)
                    if (may_read_tensor(node.source_type)) facts.effects |= HostSync | DeviceWork | MayFailDeferred;
            } else if constexpr (domain == Domain::console || domain == Domain::file ||
                                 domain == Domain::system || domain == Domain::http || domain == Domain::repl) {
                facts.effects |= ExternalIO | Alloc | MayFail;
                if constexpr (std::is_same_v<T, TimeNow> || std::is_same_v<T, TimeSince> || std::is_same_v<T, TimeSleep>)
                    facts.effects |= HostSync;
                if constexpr (std::is_same_v<T, Print> || std::is_same_v<T, ReplDisplay>)
                    if (may_read_tensor(node.type)) facts.effects |= HostSync | DeviceWork | MayFailDeferred;
                // Clock/environment/process state is retained by semantic
                // resource paths; known external effects never imply purity.
            } else if constexpr (domain == Domain::random || domain == Domain::concurrency) opaque(facts);
            else opaque(facts);
        }, instruction);
    }
    std::sort(result.callees.begin(), result.callees.end());
    result.callees.erase(std::unique(result.callees.begin(), result.callees.end()), result.callees.end());
    return result;
}
bool tensor_leaf(const Type& type, const Module& module, std::set<std::string>& seen) {
    if (type.kind == TypeKind::Tensor) return true;
    if (type.kind == TypeKind::Auto || type.kind == TypeKind::Invalid) return true;
    if (type.kind == TypeKind::Function) return false;
    if (type.first && tensor_leaf(*type.first, module, seen)) return true;
    for (const auto& child : type.parameters) if (tensor_leaf(child, module, seen)) return true;
    for (const auto& child : type.cases) if (tensor_leaf(child, module, seen)) return true;
    if (type.kind != TypeKind::Class) return false;
    if (!seen.insert(type.class_name).second) return true;
    const ClassLayout* layout = nullptr;
    for (const auto& candidate : module.classes) if (candidate.name == type.class_name) {
        if (layout) return true;
        layout = &candidate;
    }
    if (!layout) return true;
    for (const auto& field : layout->fields) if (tensor_leaf(field, module, seen)) return true;
    seen.erase(type.class_name);
    return false;
}
void result_origin(const Function& function, std::size_t index, const Module& module,
                   const ModuleSummaries& imported, AnalysisCache& cache, IrSummaryFacts& facts) {
    const auto* binding = imported.find(function.name);
    if (!binding || !binding->semantic) return;
    const auto* semantic = binding->semantic;
    // Parameter-derived values are the checker's existing value provenance.
    // This contract does not certify that copying returns independent storage.
    if (semantic->result.kind == semantics::ResultProvenance::Kind::Parameter) {
        const auto& mapping = binding->semantic_parameter_for_ir_parameter;
        std::optional<std::size_t> parameter;
        for (std::size_t ir = 0; ir < mapping.size(); ++ir) if (mapping[ir] == semantic->result.parameter) {
            if (parameter) return;
            parameter = ir;
        }
        if (parameter) { facts.result = ResultAlias::ParamDerived; facts.result_parameter = parameter; facts.result_complete = true; }
        return;
    }
    if (semantic->result.kind != semantics::ResultProvenance::Kind::Fresh) return;
    std::set<std::string> seen;
    if (!tensor_leaf(function.result, module, seen)) {
        facts.result = ResultAlias::Fresh; facts.result_complete = true; return;
    }
    const auto& cfg = cache.cfg(index);
    const auto& aliases = cache.aliases(index);
    bool has_return = false;
    for (std::size_t block = 0; block < function.blocks.size(); ++block) if (cfg.reachable[block])
        for (const auto& instruction : function.blocks[block].instructions) if (const auto* returned = std::get_if<Return>(&instruction)) {
            has_return = true;
            const auto* value = aliases.find(returned->value);
            if (!value || !value->storage_available) return;
            for (const auto id : value->classes) {
                const auto storage = std::find_if(aliases.classes.begin(), aliases.classes.end(),
                    [&](const StorageClass& candidate) { return candidate.id == id; });
                if (storage == aliases.classes.end() || !storage->alias_available || storage->origins.empty() ||
                    std::any_of(storage->origins.begin(), storage->origins.end(),
                        [](StorageOrigin origin) { return origin != StorageOrigin::Fresh; })) return;
            }
        }
    if (has_return) { facts.result = ResultAlias::Fresh; facts.result_complete = true; }
}
} // namespace

IrSummaryInputs infer_ir_summary_facts(const Module& module, const ModuleSummaries& imported) {
    IrSummaryInputs result;
    std::map<std::string, Body, std::less<>> bodies;
    std::map<std::string, std::vector<std::string>, std::less<>> callers;
    AnalysisCache cache(module);
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        const auto& function = module.functions[index];
        auto own = body(function);
        const auto* binding = imported.find(function.name);
        const auto* semantic = binding ? binding->semantic : nullptr;
        if (!semantic) opaque(own.facts);
        else if (!semantic->resources.empty()) own.facts.effects |= ExternalIO;
        if (!bodies.emplace(function.name, own).second)
            throw std::invalid_argument("IR summary function identity is duplicated");
        result.emplace(function.name, own.facts);
        for (const auto& callee : own.callees) callers[callee].push_back(function.name);
    }
    std::deque<std::string> pending;
    std::set<std::string> queued;
    for (const auto& [name, value] : bodies) { (void)value; pending.push_back(name); queued.insert(name); }
    while (!pending.empty()) {
        auto name = std::move(pending.front()); pending.pop_front(); queued.erase(name);
        const auto& own = bodies.at(name);
        auto updated = own.facts;
        for (const auto& callee : own.callees) {
            const auto called = result.find(callee);
            if (called == result.end()) opaque(updated);
            else {
                updated.effects |= called->second.effects;
                updated.effects_complete &= called->second.effects_complete;
                updated.descriptor_effects |= called->second.descriptor_effects;
                updated.descriptor_complete &= called->second.descriptor_complete;
            }
        }
        auto& previous = result.at(name);
        if (updated.effects != previous.effects || updated.effects_complete != previous.effects_complete ||
            updated.descriptor_effects != previous.descriptor_effects || updated.descriptor_complete != previous.descriptor_complete) {
            previous = updated;
            for (const auto& caller : callers[name]) if (queued.insert(caller).second) pending.push_back(caller);
        }
    }
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        const auto& function = module.functions[index];
        auto& facts = result.at(function.name);
        if (!function.external_symbol && facts.effects_complete)
            result_origin(function, index, module, imported, cache, facts);
    }
    return result;
}
} // namespace quidra::ir::analysis
