#include "ir/analysis/local_ssa.hpp"
#include "ir/analysis/inline_scopes.hpp"
#include "quidra/ir/visit.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace quidra::ir::analysis {
namespace {
void add_slot(Slots& slots, const std::string& name, const Type& type) {
    const auto [entry, inserted] = slots.type.emplace(name, type);
    if (!inserted && entry->second != type)
        throw std::invalid_argument("IR local slot type disagrees for '" + name + "'");
    if (inserted) { slots.ordered.push_back(name); slots.facts.emplace(name, SlotFacts{}); }
}
SlotFacts& facts_for(Slots& slots, std::string_view name) {
    const auto found = slots.facts.find(std::string(name));
    if (found == slots.facts.end())
        throw std::invalid_argument("IR local slot is undeclared: '" + std::string(name) + "'");
    return found->second;
}
void check_type(const Slots& slots, const std::string& name, const Type& type) {
    const auto found = slots.type.find(name);
    if (found == slots.type.end() || found->second != type)
        throw std::invalid_argument("IR local access type disagrees for '" + name + "'");
}
struct Exposures final : OperandVisitor {
    explicit Exposures(Slots& value) : slots(value) {}
    void local(std::string_view name, LocalRole role) override {
        auto& facts = facts_for(slots, name);
        if (role == LocalRole::Address) facts.address_taken = true;
        if (role == LocalRole::ReferenceRead || role == LocalRole::ReferenceWrite) facts.reference_bound = true;
    }
    Slots& slots;
};
ReachingDef undefined() { ReachingDef value; value.available = true; return value; }
ReachingDef phi_definition(PhiId id) { auto value = undefined(); value.kind = ReachingDef::Phi; value.phi = id; return value; }
struct Events {
    std::set<BlockId> definitions, uses_before_definition;
};
} // namespace

bool SlotFacts::promotable() const noexcept {
    return !writable_parameter && !borrowed_parameter && !borrowed_local &&
        !address_taken && !reference_bound && !reference_binding && !opaque_access;
}
bool Slots::promotable(std::string_view local) const {
    const auto name = std::string(local);
    const auto found = facts.find(name);
    const auto classification = cls.find(name);
    return found != facts.end() && classification != cls.end() &&
        classification->second == SlotClass::Promotable && found->second.promotable();
}
Slots classify_slots(const Function& function) {
    Slots slots;
    std::unordered_set<std::string> parameters;
    for (const auto& parameter : function.parameters) {
        if (!parameters.insert(parameter.name).second)
            throw std::invalid_argument("IR duplicate local parameter '" + parameter.name + "'");
        add_slot(slots, parameter.name, parameter.type);
        auto& facts = slots.facts.at(parameter.name);
        facts.writable_parameter = parameter.writable;
        facts.borrowed_parameter = parameter.borrowed;
    }
    std::set<std::string> scoped_names;
    for(const auto& scope:function.inline_scopes){(void)inline_scope(function,scope.identity);
        for(const auto& slot:scope.slots){
            if(parameters.contains(slot.name) || !scoped_names.insert(slot.name).second)
                throw std::invalid_argument("IR inline storage has multiple lexical owners");
            add_slot(slots,slot.name,slot.type);auto& f=slots.facts.at(slot.name);
            if(slot.kind==InlineSlotKind::BorrowedLocal)f.borrowed_local=true;
            if(slot.kind==InlineSlotKind::PinnedReference)f.reference_binding=true;
            if(slot.kind==InlineSlotKind::RawAddress){f.writable_parameter=f.borrowed_parameter=f.address_taken=true;}
        }
    }
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        if(const auto* n=std::get_if<InlineBindAddress>(&instruction)){
            add_slot(slots,n->name,n->type);auto& f=slots.facts.at(n->name);
            f.writable_parameter=f.borrowed_parameter=f.address_taken=true;
        }
        if (const auto* n = std::get_if<DeclareLocal>(&instruction)) add_slot(slots, n->name, n->type);
        // LLVM's storage pre-pass also declares typed StoreLocal slots, e.g.
        // the hidden boolean introduced by finite-check merging.
        if (const auto* n = std::get_if<StoreLocal>(&instruction)) add_slot(slots, n->name, n->type);
        if (const auto* n = std::get_if<DeclareReference>(&instruction)) {
            add_slot(slots, n->name, n->type); slots.facts.at(n->name).reference_binding = true;
        }
    }
    Exposures exposure(slots);
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        visit_operands(instruction, exposure);
        if (const auto* n = std::get_if<StoreLocal>(&instruction)) {
            check_type(slots, n->name, n->type); facts_for(slots, n->name).borrowed_local |= n->borrowed;
        }
        if (const auto* n = std::get_if<LoadLocal>(&instruction)) {
            check_type(slots, n->name, n->type); facts_for(slots, n->name).move_out |= n->move_out;
        }
        if (const auto* n = std::get_if<BindReference>(&instruction)) facts_for(slots, n->name).reference_bound = true;
        if (const auto* n = std::get_if<ReferenceAddress>(&instruction)) facts_for(slots, n->name).reference_bound = true;
        if (const auto* n = std::get_if<LoadReference>(&instruction)) check_type(slots, n->name, n->type);
        if (const auto* n = std::get_if<StoreReference>(&instruction)) check_type(slots, n->name, n->type);
        const auto opaque_flag = [&](const std::string& name) {
            check_type(slots, name, Type::simple(TypeKind::Bool));
            facts_for(slots, name).opaque_access = true;
        };
        if (const auto* n = std::get_if<FiniteFlagUpdate>(&instruction)) opaque_flag(n->flag);
        if (const auto* n = std::get_if<FiniteFlagCheck>(&instruction)) opaque_flag(n->flag);
    }
    for (const auto& name : slots.ordered) {
        const auto& facts = slots.facts.at(name);
        const auto classification = facts.writable_parameter ? SlotClass::WritableParam :
            facts.borrowed_parameter ? SlotClass::BorrowedParam : facts.borrowed_local ? SlotClass::BorrowedLocal :
            facts.reference_bound || facts.reference_binding ? SlotClass::ReferenceBound :
            facts.address_taken ? SlotClass::AddressTaken : facts.opaque_access ? SlotClass::OpaqueAccess : SlotClass::Promotable;
        slots.cls.emplace(name, classification);
    }
    return slots;
}

ReachingDef LocalSsa::reaching(InstrRef at, std::string_view local) const {
    if (at.block >= reachable_.size() || !reachable_[at.block] || at.index > instruction_count_[at.block]) return {};
    const auto name = std::string(local);
    const auto initial = initial_.find(name);
    if (initial == initial_.end()) return {};
    const auto sources = sources_.find(name);
    if (sources != sources_.end()) {
        const auto source = sources->second.find(at.block);
        if (source != sources->second.end()) {
            const auto& loads = source->second.loads;
            const auto load = std::lower_bound(loads.begin(), loads.end(), at.index,
                [](const auto& definition, std::uint32_t index) { return definition.first < index; });
            if (load != loads.end() && load->first == at.index) return load->second;
            const auto& definitions = source->second.definitions;
            const auto end = std::lower_bound(definitions.begin(), definitions.end(), at.index,
                [](const auto& definition, std::uint32_t index) { return definition.first < index; });
            if (end != definitions.begin()) return std::prev(end)->second;
            if (source->second.phi) return *source->second.phi;
        }
    } else return initial->second;
    // Pruning can remove a dead join's phi. Walk actual incoming paths rather
    // than inventing the nearest dominating definition for its old state.
    std::optional<ReachingDef> candidate;
    const auto merge = [&](const ReachingDef& definition) {
        if (!candidate) { candidate = definition; return true; }
        return *candidate == definition;
    };
    if (at.block == 0) candidate = initial->second;
    auto pending = predecessors_[at.block];
    std::vector<bool> seen(reachable_.size());
    while (!pending.empty()) {
        const auto block = pending.back(); pending.pop_back();
        if (!reachable_[block] || seen[block]) continue;
        seen[block] = true;
        if (sources != sources_.end()) {
            const auto source = sources->second.find(block);
            if (source != sources->second.end()) {
                const auto& definitions = source->second.definitions;
                if (!definitions.empty()) { if (!merge(definitions.back().second)) return {}; continue; }
                if (source->second.phi) { if (!merge(*source->second.phi)) return {}; continue; }
            }
        }
        if (block == 0 && !merge(initial->second)) return {};
        pending.insert(pending.end(), predecessors_[block].begin(), predecessors_[block].end());
    }
    return candidate.value_or(ReachingDef{});
}

LocalSsa build_local_ssa(const Function& function, const Cfg& cfg, const DomTree& dom, const Slots& slots) {
    const auto count = function.blocks.size();
    if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        function.parameters.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("IR local SSA domain is too large");
    if (cfg.succ.size() != count || cfg.pred.size() != count || cfg.reachable.size() != count ||
        dom.block_count != count || dom.scope != DomScope::EntryPaths || dom.idom.size() != count ||
        dom.frontier.size() != count || dom.children.size() != count ||
        (count && (!dom.root || *dom.root != 0)))
        throw std::invalid_argument("IR local SSA requires the current entry-path analyses");
    LocalSsa result;
    result.predecessors_ = cfg.pred; result.reachable_ = cfg.reachable;
    for (const auto& block : function.blocks) {
        if (block.instructions.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("IR local SSA block has too many instructions");
        result.instruction_count_.push_back(static_cast<std::uint32_t>(block.instructions.size()));
    }
    std::vector<std::string> names;
    std::unordered_map<std::string, std::size_t> local_index;
    for (const auto& name : slots.ordered) if (slots.promotable(name)) {
        local_index.emplace(name, names.size()); names.push_back(name); result.initial_.emplace(name, undefined());
    }
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        auto found = result.initial_.find(function.parameters[i].name);
        if (found != result.initial_.end()) { found->second.kind = ReachingDef::Param; found->second.parameter = static_cast<std::uint32_t>(i); }
    }
    std::vector<Events> events(names.size());
    for (BlockId block = 0; block < count; ++block) {
        std::unordered_set<std::size_t> defined;
        for (const auto& instruction : function.blocks[block].instructions) {
            if (const auto* n = std::get_if<LoadLocal>(&instruction)) {
                const auto found = local_index.find(n->name);
                if (found == local_index.end()) continue;
                if (!result.load_source.emplace(n->out, ReachingDef{}).second)
                    throw std::invalid_argument("IR local SSA load has duplicate output");
                if (!cfg.reachable[block]) continue;
                auto& event = events[found->second];
                if (!defined.contains(found->second)) event.uses_before_definition.insert(block);
                if (n->move_out) { event.definitions.insert(block); defined.insert(found->second); }
            }
            const InlineScope* boundary=nullptr;
            if(const auto* n=std::get_if<InlineScopeBegin>(&instruction))boundary=&inline_scope(function,n->scope);
            if(const auto* n=std::get_if<InlineScopeCleanup>(&instruction))boundary=&inline_scope(function,n->scope);
            if(boundary)for(const auto& slot:boundary->slots)if(const auto found=local_index.find(slot.name);found!=local_index.end() && cfg.reachable[block]){
                events[found->second].definitions.insert(block);defined.insert(found->second);
            }
            if (const auto* n = std::get_if<StoreLocal>(&instruction)) {
                const auto found = local_index.find(n->name);
                if (found != local_index.end() && cfg.reachable[block]) {
                    events[found->second].definitions.insert(block); defined.insert(found->second);
                }
            }
        }
    }
    std::vector<std::pair<BlockId, std::size_t>> planned;
    for (std::size_t local = 0; local < names.size(); ++local) {
        const auto& event = events[local];
        auto live_in = event.uses_before_definition;
        std::deque<BlockId> pending(live_in.begin(), live_in.end());
        while (!pending.empty()) {
            const auto block = pending.front(); pending.pop_front();
            for (const auto predecessor : cfg.pred[block]) {
                if (predecessor >= count) throw std::invalid_argument("IR local SSA predecessor is out of range");
                if (!cfg.reachable[predecessor] || event.definitions.contains(predecessor)) continue;
                if (live_in.insert(predecessor).second) pending.push_back(predecessor);
            }
        }
        // External parameter/undefined entry definitions are not definitions
        // in a looping root block; seeding root would invent unchanged phis.
        const auto& definitions = event.definitions;
        pending.assign(definitions.begin(), definitions.end());
        std::set<BlockId> placed;
        while (!pending.empty()) {
            const auto block = pending.front(); pending.pop_front();
            for (const auto frontier : dom.frontier[block]) {
                if (frontier >= count) throw std::invalid_argument("IR local SSA frontier is out of range");
                if (!live_in.contains(frontier) || !placed.insert(frontier).second) continue;
                planned.emplace_back(frontier, local);
                if (!definitions.contains(frontier)) pending.push_back(frontier);
            }
        }
    }
    std::sort(planned.begin(), planned.end());
    if (planned.size() >= std::numeric_limits<std::uint32_t>::max()) throw std::length_error("IR local SSA has too many phis");
    std::vector<std::vector<std::size_t>> block_phis(count);
    std::vector<std::unordered_map<BlockId, std::size_t>> incoming_positions(count);
    for (const auto& [block, local] : planned) {
        const auto id = PhiId{static_cast<std::uint32_t>(result.phis.size())};
        LocalSsa::Phi phi{block, names[local], id, {}, {}};
        for (const auto predecessor : cfg.pred[block]) {
            incoming_positions[block].emplace(predecessor, phi.incoming.size());
            phi.incoming.emplace_back(predecessor, ReachingDef{});
        }
        if (block == 0) phi.entry = result.initial_.at(names[local]);
        block_phis[block].push_back(result.phis.size()); result.phis.push_back(std::move(phi));
        result.sources_[names[local]][block].phi = phi_definition(id);
    }
    if (!count) return result;
    std::vector<std::vector<ReachingDef>> stacks;
    for (const auto& name : names) stacks.push_back({result.initial_.at(name)});
    struct Frame { BlockId block; std::size_t next{}; std::vector<std::size_t> pushes; bool entered{}; };
    std::vector<Frame> traversal{{0, 0, {}, false}};
    while (!traversal.empty()) {
        auto& frame = traversal.back();
        if (!frame.entered) {
            frame.entered = true;
            for (const auto phi : block_phis[frame.block]) {
                const auto local = local_index.at(result.phis[phi].local);
                stacks[local].push_back(phi_definition(result.phis[phi].id)); frame.pushes.push_back(local);
            }
            const auto& instructions = function.blocks[frame.block].instructions;
            for (std::uint32_t index = 0; index < instructions.size(); ++index) {
                const auto push = [&](const std::string& name, ReachingDef definition) {
                    const auto local = local_index.at(name); stacks[local].push_back(definition); frame.pushes.push_back(local);
                    result.sources_[name][frame.block].definitions.emplace_back(index, std::move(definition));
                };
                if (const auto* n = std::get_if<LoadLocal>(&instructions[index])) {
                    const auto local = local_index.find(n->name);
                    if (local == local_index.end()) continue;
                    result.load_source.at(n->out) = stacks[local->second].back();
                    result.sources_[n->name][frame.block].loads.emplace_back(index, stacks[local->second].back());
                    if (n->move_out) {
                        auto definition = undefined(); definition.kind = ReachingDef::Empty;
                        definition.store = {frame.block, index}; definition.empty_type = n->type; push(n->name, std::move(definition));
                    }
                }
                const InlineScope* boundary=nullptr;bool begin=false;
                if(const auto* n=std::get_if<InlineScopeBegin>(&instructions[index])){boundary=&inline_scope(function,n->scope);begin=true;}
                if(const auto* n=std::get_if<InlineScopeCleanup>(&instructions[index]))boundary=&inline_scope(function,n->scope);
                if(boundary)for(const auto& slot:boundary->slots)if(local_index.contains(slot.name)){
                    auto definition=undefined();definition.store={frame.block,index};
                    if(begin && (owns_managed(slot.type) || slot.type.kind==TypeKind::Real || is_bare_integer(slot.type))){
                        definition.kind=ReachingDef::Empty;definition.empty_type=slot.type;
                    }
                    push(slot.name,std::move(definition));
                }
                if (const auto* n = std::get_if<StoreLocal>(&instructions[index]); n && local_index.contains(n->name)) {
                    auto definition = undefined(); definition.kind = ReachingDef::Store;
                    definition.value = n->value; definition.store = {frame.block, index}; push(n->name, std::move(definition));
                }
            }
            for (const auto successor : cfg.succ[frame.block]) {
                if (successor >= count) throw std::invalid_argument("IR local SSA successor is out of range");
                for (const auto phi : block_phis[successor]) {
                    auto& record = result.phis[phi];
                    const auto source = stacks[local_index.at(record.local)].back();
                    record.incoming.at(incoming_positions[successor].at(frame.block)).second = source;
                }
            }
        }
        if (frame.next < dom.children[frame.block].size()) {
            const auto child = dom.children[frame.block][frame.next++];
            if (child >= count) throw std::invalid_argument("IR local SSA child is out of range");
            traversal.push_back({child, 0, {}, false});
        } else {
            for (auto it = frame.pushes.rbegin(); it != frame.pushes.rend(); ++it) stacks[*it].pop_back();
            traversal.pop_back();
        }
    }
    return result;
}

} // namespace quidra::ir::analysis
