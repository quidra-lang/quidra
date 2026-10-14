#include "ir/analysis/liveness.hpp"
#include "ir/analysis/inline_scopes.hpp"
#include <set>

#include <algorithm>
#include <deque>
#include <iterator>
#include <stdexcept>

namespace quidra::ir::analysis {
namespace {
template<class T> void insert(std::vector<T>& values, const T& value) {
    const auto at = std::lower_bound(values.begin(), values.end(), value);
    if (at == values.end() || *at != value) values.insert(at, value);
}
template<class T> void merge(std::vector<T>& values, const std::vector<T>& other) {
    std::vector<T> result;
    result.reserve(values.size() + other.size());
    std::set_union(values.begin(), values.end(), other.begin(), other.end(), std::back_inserter(result));
    values = std::move(result);
}
template<class T> void subtract(std::vector<T>& values, const std::vector<T>& removed) {
    std::vector<T> result;
    result.reserve(values.size());
    std::set_difference(values.begin(), values.end(), removed.begin(), removed.end(), std::back_inserter(result));
    values = std::move(result);
}
void merge_roots(LiveSet& values, const LiveSet& other) {
    merge(values.values, other.values);
    merge(values.locals, other.locals);
}
bool same_roots(const LiveSet& a, const LiveSet& b) {
    return a.values == b.values && a.locals == b.locals;
}
bool cleanup_type(const Type& type) {
    if (owns_managed(type) || type.kind == TypeKind::Real || is_bare_integer(type)) return true;
    if (type.kind == TypeKind::Function) return false;
    return std::any_of(type.parameters.begin(), type.parameters.end(), cleanup_type);
}
bool may_contain_tensor(const Type& type) {
    if (type.kind == TypeKind::Tensor || type.kind == TypeKind::Class ||
        type.kind == TypeKind::Auto || type.kind == TypeKind::Invalid) return true;
    if (type.kind == TypeKind::Function) return false;
    if (type.first && may_contain_tensor(*type.first)) return true;
    return std::any_of(type.parameters.begin(), type.parameters.end(), may_contain_tensor) ||
        std::any_of(type.cases.begin(), type.cases.end(), may_contain_tensor);
}
struct Effects final : OperandVisitor {
    LiveSet reads, writes;
    void local(std::string_view name, LocalRole role) override {
        const auto key = std::string(name);
        switch (role) {
        case LocalRole::Declaration: break; // Slots are allocated/initialized at entry.
        case LocalRole::Write: insert(writes.locals, key); break;
        case LocalRole::ReadWrite: insert(writes.locals, key); [[fallthrough]];
        case LocalRole::Read:
        case LocalRole::Address:
        case LocalRole::ReferenceRead:
        case LocalRole::ReferenceWrite: insert(reads.locals, key); break;
        }
    }
};
struct InstructionEffects {
    LiveSet reads, writes;
    std::vector<BlockId> failure;
    LiveSet failure_cleanup;
};
} // namespace

const LiveSet* Liveness::before(InstrRef at) const noexcept {
    if (at.block >= blocks.size() || at.index >= blocks[at.block].before.size()) return nullptr;
    return &blocks[at.block].before[at.index];
}
const LiveSet* Liveness::after(InstrRef at) const noexcept {
    if (at.block >= blocks.size() || at.index >= blocks[at.block].after.size()) return nullptr;
    return &blocks[at.block].after[at.index];
}

Liveness liveness(const Function& function, const Cfg& cfg, const DefUse& du,
                  const Slots& slots, const AliasInfo& aliases, const LivenessOptions& options,
                  const LocalSsa* local_ssa) {
    const auto count = function.blocks.size();
    if (count > std::numeric_limits<BlockId>::max())
        throw std::length_error("IR liveness block index overflow");
    if (cfg.succ.size() != count || cfg.pred.size() != count || cfg.reachable.size() != count)
        throw std::invalid_argument("IR liveness requires current CFG dimensions");
    Liveness result;
    result.blocks.resize(count);
    result.storage_complete = aliases.complete;
    result.implicit_failures_conservative = options.conservative_failure_cleanup;
    std::vector<std::vector<InstructionEffects>> effects(count);
    std::vector<std::vector<BlockId>> dependents = cfg.pred;
    for (auto& predecessors : dependents) {
        std::sort(predecessors.begin(), predecessors.end());
        predecessors.erase(std::unique(predecessors.begin(), predecessors.end()), predecessors.end());
    }
    std::set<std::string> scoped_names;
    for(const auto& scope:function.inline_scopes)for(const auto& slot:scope.slots)scoped_names.insert(slot.name);
    for(const auto& block:function.blocks)for(const auto& ins:block.instructions)if(const auto* n=std::get_if<InlineBindAddress>(&ins))scoped_names.insert(n->name);
    LiveSet cleanup;
    for (const auto& name : slots.ordered) {
        if(scoped_names.contains(name))continue;
        const auto& facts = slots.facts.at(name);
        if (facts.writable_parameter || facts.reference_binding ||
            (!facts.borrowed_parameter && !facts.borrowed_local && cleanup_type(slots.type.at(name))))
            insert(cleanup.locals, name);
    }
    for (std::size_t block = 0; block < count; ++block) {
        const auto& instructions = function.blocks[block].instructions;
        if (instructions.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("IR liveness instruction index overflow");
        result.blocks[block].reachable = cfg.reachable[block];
        effects[block].resize(instructions.size());
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            Effects visitor;
            visit_operands(instructions[index], visitor);
            if (const auto* store = std::get_if<StoreLocal>(&instructions[index]);
                store && !store->borrowed && !store->replace_without_release && cleanup_type(store->type))
                insert(visitor.reads.locals, store->name);
            if (const auto* binding = std::get_if<BindReference>(&instructions[index]))
                insert(visitor.reads.locals, binding->name); // Rebinding unpins the old target.
            if(const auto* binding=std::get_if<InlineBindAddress>(&instructions[index]))insert(visitor.writes.locals,binding->name);
            const InlineScope* boundary=nullptr;bool begin=false;
            if(const auto* n=std::get_if<InlineScopeBegin>(&instructions[index])){boundary=&inline_scope(function,n->scope);begin=true;}
            if(const auto* n=std::get_if<InlineScopeCleanup>(&instructions[index]))boundary=&inline_scope(function,n->scope);
            if(boundary)for(const auto& slot:boundary->slots){
                insert(visitor.writes.locals,slot.name); // Logical cell lifetime ends/restarts.
                if(!begin && (slot.kind==InlineSlotKind::PinnedReference ||
                    (slot.kind==InlineSlotKind::OwnedLocal && cleanup_type(slot.type))))insert(visitor.reads.locals,slot.name);
            }
            for (const auto& name : visitor.reads.locals) if (!slots.type.contains(name))
                throw std::invalid_argument("IR liveness read names an unknown local");
            for (const auto& name : visitor.writes.locals) if (!slots.type.contains(name))
                throw std::invalid_argument("IR liveness write names an unknown local");
            effects[block][index].reads = std::move(visitor.reads);
            effects[block][index].writes = std::move(visitor.writes);
        }
        for (const auto successor : cfg.succ[block]) if (successor >= count)
            throw std::invalid_argument("IR liveness successor is out of range");
        for (const auto predecessor : cfg.pred[block]) if (predecessor >= count)
            throw std::invalid_argument("IR liveness predecessor is out of range");
    }
    // A may-active scope meet includes error cleanup only while its lexical
    // frame may exist. Dead child slots never become caller exit obligations.
    std::vector<std::set<std::uint64_t>> active_out(count);
    bool active_changed=true;
    while(active_changed){active_changed=false;for(const auto block:cfg.rpo){
        std::set<std::uint64_t> active;
        for(const auto pred:cfg.pred[block])active.insert(active_out[pred].begin(),active_out[pred].end());
        for(std::size_t i=0;i<function.blocks[block].instructions.size();++i){
            auto& effect=effects[block][i];effect.failure_cleanup=cleanup;
            const auto& ins=function.blocks[block].instructions[i];
            if(std::holds_alternative<InlineExit>(ins))effect.failure_cleanup={};
            else for(const auto identity:active)for(const auto& slot:inline_scope(function,identity).slots)
                if(slot.kind==InlineSlotKind::PinnedReference || (slot.kind==InlineSlotKind::OwnedLocal && cleanup_type(slot.type)))insert(effect.failure_cleanup.locals,slot.name);
            if(const auto* n=std::get_if<InlineScopeBegin>(&ins))active.insert(n->scope);
            if(const auto* n=std::get_if<InlineScopeCleanup>(&ins))active.erase(n->scope);
        }
        if(active_out[block]!=active){active_out[block]=std::move(active);active_changed=true;}
    }}
    const auto at = [&](InstrRef site) -> InstructionEffects& {
        if (site.block >= count || site.index >= effects[site.block].size())
            throw std::invalid_argument("IR liveness instruction site is out of range");
        return effects[site.block][site.index];
    };
    for (const auto& record : du.values) {
        if (!record.value) throw std::invalid_argument("IR liveness cannot analyze a zero SSA operand");
        for (const auto definition : record.definitions) insert(at(definition).writes.values, record.value);
        for (const auto& use : record.uses) {
            insert(at(use.at).reads.values, record.value);
            result.roles_complete &= use.role_available;
        }
    }
    for (const auto& edge : options.failure_edges) {
        if (edge.target >= count) throw std::invalid_argument("IR liveness failure target is out of range");
        insert(at(edge.at).failure, edge.target);
        insert(dependents[edge.target], edge.at.block);
    }
    const auto transfer = [&](const InstructionEffects& instruction, LiveSet live) {
        subtract(live.values, instruction.writes.values);
        subtract(live.locals, instruction.writes.locals);
        merge_roots(live, instruction.reads);
        for (const auto target : instruction.failure) merge_roots(live, result.blocks[target].in);
        if (options.conservative_failure_cleanup) merge_roots(live, instruction.failure_cleanup);
        return live;
    };
    std::deque<BlockId> pending;
    std::vector<bool> queued(count, true);
    for (std::size_t block = count; block > 0; --block) pending.push_back(static_cast<BlockId>(block - 1));
    while (!pending.empty()) {
        const auto block = pending.front(); pending.pop_front(); queued[block] = false;
        LiveSet out;
        if (cfg.succ[block].empty() && (function.blocks[block].instructions.empty() ||
            !std::holds_alternative<InlineExit>(function.blocks[block].instructions.back()))) out = cleanup;
        for (const auto successor : cfg.succ[block]) merge_roots(out, result.blocks[successor].in);
        auto in = out;
        for (auto index = effects[block].size(); index > 0; --index) in = transfer(effects[block][index - 1], std::move(in));
        const auto changed = !same_roots(in, result.blocks[block].in);
        result.blocks[block].out = std::move(out);
        result.blocks[block].in = std::move(in);
        if (changed) for (const auto predecessor : dependents[block]) if (!queued[predecessor]) {
            queued[predecessor] = true; pending.push_back(predecessor);
        }
    }
    std::vector<StorageClassId> retained, all_storage;
    if (!std::is_sorted(aliases.values.begin(), aliases.values.end(),
            [](const StorageValue& a, const StorageValue& b) { return a.value < b.value; }))
        throw std::invalid_argument("IR liveness requires sorted alias values");
    for (const auto& storage : aliases.classes) {
        insert(all_storage, storage.id);
        if (!storage.owned || storage.borrowed || storage.may_have_other_owners || storage.reaches_autograd ||
            storage.address_exposed || storage.escapes_function || !storage.alias_available ||
            !storage.ownership_available || !storage.state_identity_available)
            insert(retained, storage.id);
    }
    const auto project = [&](LiveSet& live, InstrRef point) {
        live.storage = retained;
        const auto classes = [&](const std::vector<StorageClassId>& ids) {
            for (const auto id : ids) {
                if (!std::binary_search(all_storage.begin(), all_storage.end(), id))
                    throw std::invalid_argument("IR liveness storage class is absent");
                insert(live.storage, id);
            }
        };
        for (const auto value : live.values) {
            const auto found = std::lower_bound(aliases.values.begin(), aliases.values.end(), value,
                [](const StorageValue& entry, ValueId key) { return entry.value < key; });
            if (found == aliases.values.end() || found->value != value) {
                // An absent value has no type/storage proof. Empty AliasInfo is
                // valid for scalar-only clients, but cannot certify donation.
                live.storage_available = false;
            } else {
                live.storage_available &= found->storage_available;
                classes(found->classes);
            }
        }
        for (const auto& name : live.locals) {
            if (!may_contain_tensor(slots.type.at(name))) continue;
            if (slots.promotable(name) && local_ssa) {
                const auto storage = aliases.storage_of(local_ssa->reaching(point, name));
                classes(storage.classes);
                live.storage_available &= storage.storage_available;
            } else {
                const auto found = aliases.slot_storage.find(name);
                if (found != aliases.slot_storage.end()) {
                    classes(found->second.classes);
                    live.storage_available &= found->second.storage_available;
                } else live.storage_available = false;
            }
        }
        if (!live.storage_available) live.storage = all_storage;
        result.storage_complete &= live.storage_available;
    };
    for (std::size_t block = 0; block < count; ++block) {
        auto& output = result.blocks[block];
        auto live = output.out;
        output.before.resize(effects[block].size()); output.after.resize(effects[block].size());
        for (auto index = effects[block].size(); index > 0; --index) {
            output.after[index - 1] = live;
            live = transfer(effects[block][index - 1], std::move(live));
            output.before[index - 1] = live;
        }
        const auto id = static_cast<BlockId>(block);
        project(output.in, {id, 0});
        project(output.out, {id, static_cast<std::uint32_t>(effects[block].size())});
        for (std::uint32_t index = 0; index < output.before.size(); ++index) {
            project(output.before[index], {id, index});
            project(output.after[index], {id, index + 1});
        }
    }
    return result;
}

} // namespace quidra::ir::analysis
