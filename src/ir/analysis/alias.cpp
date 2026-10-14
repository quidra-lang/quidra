#include "ir/analysis/alias.hpp"
#include "ir/analysis/inline_scopes.hpp"
#include "quidra/ir/module.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace quidra::ir::analysis {
namespace {
enum class Contains { No, Yes, Unknown };
template<class T> void append(std::vector<T>& values, const T& value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}
Contains combine(Contains a, Contains b) {
    if (a == Contains::Unknown || b == Contains::Unknown) return Contains::Unknown;
    return a == Contains::Yes || b == Contains::Yes ? Contains::Yes : Contains::No;
}
struct Types {
    const Module* module;
    Contains contains(const Type& type, std::set<std::string>& active) const {
        if (type.kind == TypeKind::Tensor) return Contains::Yes;
        if (type.kind == TypeKind::Auto || type.kind == TypeKind::Invalid) return Contains::Unknown;
        // Callable parameter/result types are a signature, not captured values.
        if (type.kind == TypeKind::Function) return Contains::No;
        if (type.kind == TypeKind::Array || type.kind == TypeKind::Traversal)
            return type.first ? contains(*type.first, active) : Contains::Unknown;
        Contains result = Contains::No;
        if (type.kind == TypeKind::Tuple) {
            for (const auto& child : type.parameters) result = combine(result, contains(child, active));
        } else if (type.kind == TypeKind::Union) {
            for (const auto& child : type.cases) result = combine(result, contains(child, active));
        } else if (type.kind == TypeKind::Class) {
            if (!module || !active.insert(type.class_name).second) return Contains::Unknown;
            const ClassLayout* layout = nullptr;
            for (const auto& candidate : module->classes) if (candidate.name == type.class_name) {
                if (layout) { active.erase(type.class_name); return Contains::Unknown; }
                layout = &candidate;
            }
            if (!layout) result = Contains::Unknown;
            else for (std::size_t i = 0; i < layout->fields.size(); ++i) {
                const auto child = contains(layout->fields[i], active);
                // Reference targets are not exported by the present typed IR.
                result = combine(result, layout->reference_fields.contains(i) && child != Contains::No
                    ? Contains::Unknown : child);
            }
            active.erase(type.class_name);
        }
        return result;
    }
    Contains contains(const Type& type) const { std::set<std::string> active; return contains(type, active); }
};
struct Variable {
    Contains contains{Contains::No};
    bool tensor{}, storage_available{true}, borrowed{}, descriptor_owned{}, foreign{};
    bool ownership_available{true};
    std::set<std::size_t> storage;
    std::vector<std::size_t> outgoing;
    StorageClass facts;
};
struct Seed { StorageClass facts; std::size_t parent{}; };
struct Builder {
    const Function& function;
    const DefUse& du;
    const LocalSsa& ssa;
    const Slots& slots;
    Types types;
    std::vector<Variable> variables;
    std::map<ValueId, std::size_t> value;
    std::map<std::string, std::size_t> slot;
    std::map<std::uint32_t, std::size_t> parameter, phi;
    // Addresses identify possible places; they are not TensorStorage values.
    std::map<ValueId, std::set<std::size_t>> addresses;
    std::map<std::string, std::set<std::size_t>> references;
    std::set<std::string> raw_slots;
    std::vector<Seed> seeds;
    bool complete{true};

    std::size_t variable(const Type& type) {
        if (variables.size() == std::numeric_limits<std::size_t>::max()) throw std::length_error("alias variable domain overflow");
        Variable v; v.contains = types.contains(type); v.tensor = type.kind == TypeKind::Tensor;
        v.storage_available = v.contains != Contains::Unknown;
        variables.push_back(std::move(v)); return variables.size() - 1;
    }
    std::size_t id(ValueId v) const { return value.at(v); }
    void type(ValueId v, const Type& t) {
        auto& n = variables[id(v)]; n.contains = types.contains(t); n.tensor = t.kind == TypeKind::Tensor;
        n.storage_available = n.contains != Contains::Unknown;
    }
    void tensor(ValueId v) { auto& n = variables[id(v)]; n.contains = Contains::Yes; n.tensor = true; }
    void origin(std::size_t v, StorageOrigin kind, std::optional<InstrRef> at = {}) {
        append(variables[v].facts.origins, kind); if (at) append(variables[v].facts.sites, *at);
    }
    void state(std::size_t v, StateSource source) {
        variables[v].facts.state_identity_available &= source.identity_available;
        append(variables[v].facts.state, source);
    }
    void seed(std::size_t v, StorageOrigin kind, bool owned, bool aliases_known,
              std::optional<InstrRef> at = {}) {
        if (variables[v].contains == Contains::No) return;
        StorageClass f; f.owned = owned; f.alias_available = aliases_known;
        f.ownership_available = kind != StorageOrigin::Unknown && kind != StorageOrigin::CallResult && kind != StorageOrigin::AutogradGrad;
        f.ownership_available &= variables[v].contains != Contains::Unknown;
        f.owned &= f.ownership_available;
        f.origins.push_back(kind); if (at) f.sites.push_back(*at);
        f.may_have_other_owners = kind != StorageOrigin::Fresh;
        f.reaches_autograd = kind != StorageOrigin::Fresh;
        f.borrowed = kind == StorageOrigin::BorrowedParam || kind == StorageOrigin::BorrowedLocal;
        const auto index = seeds.size(); seeds.push_back({std::move(f), index}); variables[v].storage.insert(index);
    }
    void unknown(std::size_t v, StorageOrigin kind, std::optional<InstrRef> at = {}) {
        if (variables[v].contains == Contains::No) return;
        seed(v, kind, false, false, at); variables[v].storage_available = false;
        variables[v].ownership_available = false; complete = false;
    }
    void flow(std::size_t to, std::size_t from) {
        if (variables[to].contains != Contains::No) append(variables[from].outgoing, to);
    }
    void flow_id(ValueId to, ValueId from) { flow(id(to), id(from)); }
    void shared(ValueId out, ValueId input, InstrRef at) {
        flow_id(out, input); variables[id(out)].descriptor_owned = true;
        variables[id(out)].facts.may_have_other_owners = true;
        append(variables[id(out)].facts.sites, at);
    }
    void fresh(ValueId out, InstrRef at, bool autograd = false, bool tensor_value = true) {
        auto& value = variables[id(out)]; value.contains = Contains::Yes; value.tensor = tensor_value;
        seed(id(out), StorageOrigin::Fresh, true, true, at);
        variables[id(out)].descriptor_owned = true; variables[id(out)].facts.reaches_autograd |= autograd;
        variables[id(out)].facts.may_have_other_owners |= autograd;
    }
    void expose(std::size_t v, bool address = false) {
        variables[v].facts.may_have_other_owners = true;
        variables[v].facts.escapes_function = true;
        variables[v].facts.address_exposed |= address;
    }
    void borrow(ValueId out, StorageOrigin kind, InstrRef at, StateSource source) {
        const auto v = id(out); variables[v].borrowed = true; variables[v].facts.borrowed = true;
        variables[v].facts.may_have_other_owners = true; origin(v, kind, at); state(v, std::move(source));
    }
    void reach(std::size_t to, const ReachingDef& definition) {
        if (!definition.available) { unknown(to, StorageOrigin::Unknown); return; }
        switch (definition.kind) {
            case ReachingDef::Store: flow(to, id(definition.value)); break;
            case ReachingDef::Param:
                if (parameter.contains(definition.parameter)) flow(to, parameter.at(definition.parameter));
                else unknown(to, StorageOrigin::Unknown);
                break;
            case ReachingDef::Phi:
                if (phi.contains(definition.phi.value)) flow(to, phi.at(definition.phi.value));
                else unknown(to, StorageOrigin::Unknown);
                break;
            case ReachingDef::Empty: break; // Typed empty/null after move-out has no payload.
            case ReachingDef::Undefined: unknown(to, StorageOrigin::Unknown); break;
        }
    }
    void initialize() {
        for(const auto& scope:function.inline_scopes)for(const auto& s:scope.slots)if(s.kind==InlineSlotKind::RawAddress)raw_slots.insert(s.name);
        for(const auto& block:function.blocks)for(const auto& ins:block.instructions)if(const auto* n=std::get_if<InlineBindAddress>(&ins))raw_slots.insert(n->name);
        for (const auto& record : du.values) {
            const auto index = variables.size(); variables.emplace_back(); value.emplace(record.value, index);
            if (!record.value || record.definitions.size() != 1) {
                variables[index].contains = Contains::Unknown; unknown(index, StorageOrigin::Unknown);
            }
        }
        for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
            std::visit([&](const auto& n) {
                using N = std::decay_t<decltype(n)>;
                if constexpr (requires { n.out; }) {
                    if (!value.contains(n.out)) return;
                    if constexpr (requires { n.result_type; }) type(n.out, n.result_type);
                    else if constexpr (requires { n.result; }) type(n.out, n.result);
                    else if constexpr (requires { n.type; }) type(n.out, n.type);
                    else if constexpr (std::is_same_v<N, FieldGet>) type(n.out, n.field_type);
                    else if constexpr (std::is_same_v<N, ArrayGet> || std::is_same_v<N, TupleGet>) type(n.out, n.element_type);
                    else if constexpr (std::is_same_v<N, VariantPayload>) type(n.out, n.payload_type);
                    else if constexpr (std::is_same_v<N, ArrayGrowMove> || std::is_same_v<N, ArraySorted> || std::is_same_v<N, ArraySlice>) type(n.out, n.array_type);
                    else if constexpr (std::is_same_v<N, NumericConvert>) type(n.out, n.target_type);
                    if constexpr (std::is_same_v<N, TensorCreate> || std::is_same_v<N, TensorTransfer> ||
                        std::is_same_v<N, TensorReshape> || std::is_same_v<N, TensorTranspose> ||
                        std::is_same_v<N, TensorContiguous> || std::is_same_v<N, TensorGather> ||
                        std::is_same_v<N, TensorScatter> || std::is_same_v<N, TensorCast> ||
                        std::is_same_v<N, TensorBinary> || std::is_same_v<N, TensorCompare> ||
                        std::is_same_v<N, TensorIndex> || std::is_same_v<N, TensorTrack> ||
                        std::is_same_v<N, TensorGrad> || std::is_same_v<N, AutogradTargetGradient>) tensor(n.out);
                }
            }, instruction);
        }
        for (const auto& name : slots.ordered) {
            const auto v = variable(slots.type.at(name)); slot.emplace(name, v);
            const auto& facts = slots.facts.at(name);
            variables[v].borrowed = facts.borrowed_parameter || facts.borrowed_local || facts.writable_parameter || facts.reference_binding;
            variables[v].facts.borrowed |= variables[v].borrowed;
            variables[v].facts.address_exposed = facts.address_taken || facts.reference_bound || facts.reference_binding || facts.writable_parameter;
            state(v, {StateSource::Kind::Local, name, {}, {}, true});
            if (!slots.promotable(name)) {
                variables[v].facts.escapes_function = true;
                if (facts.borrowed_local) origin(v, StorageOrigin::BorrowedLocal);
            }
        }
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            if (i > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("alias parameter index overflow");
            const auto& p = function.parameters[i]; const auto v = variable(p.type);
            parameter.emplace(static_cast<std::uint32_t>(i), v);
            // Const-argument isolation and reference target exports are pending.
            const bool borrowed = p.borrowed || p.writable || p.is_const;
            variables[v].borrowed = borrowed; variables[v].facts.borrowed = borrowed;
            variables[v].foreign = borrowed;
            variables[v].facts.address_exposed = p.writable;
            variables[v].descriptor_owned = !borrowed;
            seed(v, borrowed ? StorageOrigin::BorrowedParam : StorageOrigin::Param, !borrowed, false);
            state(v, {StateSource::Kind::Parameter, p.name, static_cast<std::uint32_t>(i), {}, true});
            flow(slot.at(p.name), v);
        }
        for (const auto& p : ssa.phis) {
            const auto found = slots.type.find(p.local);
            if (found == slots.type.end()) throw std::invalid_argument("alias phi local is undeclared");
            if (!phi.emplace(p.id.value, variable(found->second)).second) throw std::invalid_argument("alias phi identity duplicated");
        }
        for (const auto& p : ssa.phis) {
            const auto v = phi.at(p.id.value);
            for (const auto& [block, source] : p.incoming) { (void)block; reach(v, source); }
            if (p.entry) reach(v, *p.entry);
        }
    }
    void address_flows() {
        // Monotone sets support authored block reordering and reference cycles.
        bool changed;
        do {
            changed = false;
            const auto add = [&](auto& destination, const auto& source) {
                for (const auto v : source) changed |= destination.insert(v).second;
            };
            for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
                std::visit([&](const auto& n) {
                    using N = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<N, AddressLocal> || std::is_same_v<N, AddressReferenceSlot>) {
                        if(raw_slots.contains(n.name))add(addresses[n.out],references[n.name]);
                        else changed |= addresses[n.out].insert(slot.at(n.name)).second;
                    } else if constexpr (std::is_same_v<N, AddressField> || std::is_same_v<N, FieldReferenceAddress>) {
                        changed |= addresses[n.out].insert(id(n.object)).second;
                    } else if constexpr (std::is_same_v<N, AddressElement>) {
                        changed |= addresses[n.out].insert(id(n.array)).second;
                    } else if constexpr (std::is_same_v<N, AddressTupleElement>) {
                        add(addresses[n.out], addresses[n.address]);
                    } else if constexpr (std::is_same_v<N, InlineBindAddress>) {
                        add(references[n.name],addresses[n.address]);
                    } else if constexpr (std::is_same_v<N, BindReference>) {
                        if (n.address) add(references[n.name], addresses[n.address]);
                    } else if constexpr (std::is_same_v<N, ReferenceAddress>) {
                        add(addresses[n.out], references[n.name]);
                        if(raw_slots.contains(n.name))add(addresses[n.out],references[n.name]);
                        else changed |= addresses[n.out].insert(slot.at(n.name)).second;
                    }
                }, instruction);
            }
        } while (changed);
    }
    void instructions() {
        std::map<std::pair<BlockId, std::uint32_t>, std::vector<std::pair<ValueId, Use>>> uses;
        for (const auto& record : du.values) for (const auto& u : record.uses)
            uses[{u.at.block, u.at.index}].emplace_back(record.value, u);
        for (auto& [at, operands] : uses) { (void)at; std::sort(operands.begin(), operands.end(), [](const auto& a, const auto& b) { return a.second.ordinal < b.second.ordinal; }); }
        for (std::size_t block = 0; block < function.blocks.size(); ++block) {
            if (block > std::numeric_limits<BlockId>::max()) throw std::length_error("alias block index overflow");
            const auto& instructions = function.blocks[block].instructions;
            for (std::size_t index = 0; index < instructions.size(); ++index) {
                if (index > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("alias instruction index overflow");
                const InstrRef at{static_cast<BlockId>(block), static_cast<std::uint32_t>(index)};
                const auto& operands = uses[{at.block, at.index}];
                const auto opaque = [&] {
                    complete = false;
                    for (const auto& [v, use] : operands) {
                        // Without a summary the call may replace contained
                        // state as well as retain it. A later projection must
                        // not certify the old, closed set of tensor sources.
                        unknown(id(v), StorageOrigin::Unknown, at);
                        expose(id(v), use.role == OperandRole::Address);
                        variables[id(v)].facts.reaches_autograd = true;
                        variables[id(v)].facts.ownership_available &= use.role_available;
                        state(id(v), {StateSource::Kind::Call, {}, {}, {}, false});
                        if (use.role == OperandRole::Address) for (const auto target : addresses[v]) {
                            expose(target, true); unknown(target, StorageOrigin::Unknown, at);
                        }
                    }
                };
                std::visit([&](const auto& n) {
                    using N = std::decay_t<decltype(n)>;
                    if constexpr (requires { n.out; }) {
                        if (value.contains(n.out) && variables[id(n.out)].contains != Contains::No)
                            append(variables[id(n.out)].facts.sites, at);
                    }
                    if constexpr (std::is_same_v<N, InlineScopeCleanup>) {
                        if(inline_cleanup_unknown(function,inline_scope(function,n.scope))){
                            complete=false;
                            // A user drop can reach hidden state without an explicit
                            // operand. Do not certify closed payload sets across it.
                            for(std::size_t v=0;v<variables.size();++v)if(variables[v].contains!=Contains::No){
                                unknown(v,StorageOrigin::Unknown,at);expose(v,true);
                                state(v,{StateSource::Kind::Call,{},{},{},false});
                            }
                        }
                    } else if constexpr (std::is_same_v<N, TensorCreate>) fresh(n.out, at);
                    else if constexpr (std::is_same_v<N, TensorCast> || std::is_same_v<N, TensorGather> ||
                        std::is_same_v<N, TensorScatter> || std::is_same_v<N, TensorBinary> || std::is_same_v<N, TensorCompare>) {
                        // Current cast CPU/GPU paths allocate even for equal dtypes;
                        // no name/type-equality shortcut substitutes for that path.
                        // Recoverable casts wrap the successful fresh tensor
                        // in a Tensor|Error carrier; that carrier is not itself
                        // a tensor descriptor eligible for an owned donor.
                        if constexpr (std::is_same_v<N, TensorCast>) fresh(n.out, at, true, n.result_type.kind != TypeKind::Union);
                        else fresh(n.out, at, true);
                        for (const auto& [v, use] : operands) { (void)use; variables[id(v)].facts.reaches_autograd = true; variables[id(v)].facts.may_have_other_owners = true; }
                    } else if constexpr (std::is_same_v<N, Unary>) {
                        if (variables[id(n.out)].tensor) { fresh(n.out, at, true); variables[id(n.operand)].facts.reaches_autograd = true; }
                    } else if constexpr (std::is_same_v<N, TensorReshape> || std::is_same_v<N, TensorTranspose> ||
                        std::is_same_v<N, TensorIndex> || std::is_same_v<N, TensorContiguous> || std::is_same_v<N, TensorTransfer> || std::is_same_v<N, TensorTrack>) {
                        // Transfer may share a UnifiedBlock across distinct storage
                        // descriptors; contiguous may clone an already dense input.
                        shared(n.out, n.tensor, at);
                        if constexpr (std::is_same_v<N, TensorTrack>) {
                            if (n.mode != tensor_track_mode::untrack) variables[id(n.out)].facts.reaches_autograd = true;
                        } else if constexpr (!std::is_same_v<N, TensorTransfer> && !std::is_same_v<N, TensorContiguous>) variables[id(n.out)].facts.reaches_autograd = true;
                    } else if constexpr (std::is_same_v<N, Clone> || std::is_same_v<N, Retain>) shared(n.out, n.value, at);
                    else if constexpr (std::is_same_v<N, LoadLocal>) {
                        const auto out = id(n.out);
                        if (slots.promotable(n.name)) {
                            const auto source = ssa.load_source.find(n.out);
                            if (source == ssa.load_source.end()) unknown(out, StorageOrigin::Unknown, at);
                            else reach(out, source->second);
                        } else flow(out, slot.at(n.name));
                        variables[out].descriptor_owned = n.move_out && slots.promotable(n.name);
                        variables[out].borrowed |= !n.move_out || !slots.promotable(n.name);
                        variables[out].facts.borrowed |= variables[out].borrowed;
                        state(out, {StateSource::Kind::Local, n.name, {}, {}, true});
                        if (slots.facts.at(n.name).borrowed_local) origin(out, StorageOrigin::BorrowedLocal, at);
                    } else if constexpr (std::is_same_v<N, InlineBindAddress>) {
                        const auto& targets=addresses[n.address];
                        if(targets.empty())unknown(slot.at(n.name),StorageOrigin::ReferenceLoad,at);
                        for(const auto target:targets){flow(slot.at(n.name),target);expose(target,true);}
                    } else if constexpr (std::is_same_v<N, StoreLocal>) {
                        flow(slot.at(n.name), id(n.value));
                        if(raw_slots.contains(n.name)){
                            const auto& targets=references[n.name];
                            if(targets.empty())unknown(slot.at(n.name),StorageOrigin::ReferenceLoad,at);
                            for(const auto target:targets){flow(target,id(n.value));expose(target,true);}
                        }
                        if (n.borrowed) { variables[slot.at(n.name)].borrowed = true; origin(slot.at(n.name), StorageOrigin::BorrowedLocal, at); }
                        if (!slots.promotable(n.name)) expose(id(n.value), variables[slot.at(n.name)].facts.address_exposed);
                    } else if constexpr (std::is_same_v<N, TupleMake> || std::is_same_v<N, ArrayMake>) {
                        for (const auto v : n.elements) { flow_id(n.out, v); expose(id(v)); }
                    } else if constexpr (std::is_same_v<N, ClassMake>) {
                        for (const auto& v : n.fields) if (v) { flow_id(n.out, *v); expose(id(*v)); }
                        if (variables[id(n.out)].contains == Contains::Unknown) unknown(id(n.out), StorageOrigin::Unknown, at);
                    } else if constexpr (std::is_same_v<N, VariantMake>) {
                        if (n.payload) { flow_id(n.out, n.payload); expose(id(n.payload)); }
                    } else if constexpr (std::is_same_v<N, ArraySlice> || std::is_same_v<N, ArraySorted> ||
                        std::is_same_v<N, ArrayGrowMove> || std::is_same_v<N, ArrayNumericCast>) {
                        // Fresh outer arrays can still contain shared tensor children.
                        flow_id(n.out, n.array); variables[id(n.out)].facts.may_have_other_owners = true;
                    } else if constexpr (std::is_same_v<N, FieldGet> || std::is_same_v<N, TupleGet>) {
                        if constexpr (std::is_same_v<N, FieldGet>) flow_id(n.out, n.object);
                        else flow_id(n.out, n.tuple);
                        borrow(n.out, StorageOrigin::FieldLoad, at, {StateSource::Kind::Field, {}, {}, n.index, false});
                    } else if constexpr (std::is_same_v<N, ArrayGet>) {
                        flow_id(n.out, n.array);
                        borrow(n.out, StorageOrigin::ElementLoad, at, {StateSource::Kind::Element, {}, {}, {}, false});
                    } else if constexpr (std::is_same_v<N, VariantPayload>) {
                        flow_id(n.out, n.container);
                        borrow(n.out, StorageOrigin::FieldLoad, at, {StateSource::Kind::Field, {}, {}, {}, false});
                    } else if constexpr (std::is_same_v<N, FieldSet> || std::is_same_v<N, ArraySet>) {
                        if constexpr (std::is_same_v<N, FieldSet>) flow(id(n.object), id(n.value));
                        else flow(id(n.array), id(n.value));
                        expose(id(n.value));
                    } else if constexpr (std::is_same_v<N, ArraySliceSet>) { flow(id(n.array), id(n.value)); expose(id(n.value)); }
                    else if constexpr (std::is_same_v<N, LoadAddress>) {
                        const auto& targets = addresses[n.address];
                        if (targets.empty()) unknown(id(n.out), StorageOrigin::ReferenceLoad, at);
                        for (const auto v : targets) { flow(id(n.out), v); expose(v, true); }
                        borrow(n.out, StorageOrigin::ReferenceLoad, at, {StateSource::Kind::Reference, {}, {}, {}, false});
                    } else if constexpr (std::is_same_v<N, LoadReference>) {
                        const auto& targets = references[n.name];
                        if (targets.empty()) unknown(id(n.out), StorageOrigin::ReferenceLoad, at);
                        for (const auto v : targets) flow(id(n.out), v);
                        flow(id(n.out), slot.at(n.name));
                        borrow(n.out, StorageOrigin::ReferenceLoad, at, {StateSource::Kind::Reference, n.name, {}, {}, false});
                    } else if constexpr (std::is_same_v<N, StoreAddress> || std::is_same_v<N, StoreReference>) {
                        expose(id(n.value), true);
                        const auto store = [&](const auto& targets) {
                            if (targets.empty()) {
                                complete = false; variables[id(n.value)].facts.ownership_available = false;
                                state(id(n.value), {StateSource::Kind::Reference, {}, {}, {}, false});
                            }
                            for (const auto v : targets) flow(v, id(n.value));
                        };
                        if constexpr (std::is_same_v<N, StoreAddress>) store(addresses[n.address]);
                        else store(references[n.name]);
                    } else if constexpr (std::is_same_v<N, BindFieldReference>) {
                        for (const auto v : addresses[n.address]) flow(id(n.object), v);
                        state(id(n.object), {StateSource::Kind::Reference, {}, {}, n.index, false});
                    } else if constexpr (std::is_same_v<N, Return>) expose(id(n.value));
                    else if constexpr (std::is_same_v<N, Pin>) expose(id(n.value), true);
                    else if constexpr (std::is_same_v<N, TensorGrad> || std::is_same_v<N, AutogradTargetGradient>) {
                        unknown(id(n.out), StorageOrigin::AutogradGrad, at); variables[id(n.out)].borrowed = true;
                        variables[id(n.out)].facts.reaches_autograd = true;
                    } else if constexpr (std::is_same_v<N, Call> || std::is_same_v<N, IndirectCall>) {
                        opaque();
                        if (value.contains(n.out) && variables[id(n.out)].contains != Contains::No) {
                            unknown(id(n.out), StorageOrigin::CallResult, at);
                            for (const auto& [v, use] : operands) { (void)use; flow_id(n.out, v); }
                        }
                    } else if constexpr (std::is_same_v<N, ParallelBlock>) opaque();
                    else if constexpr (requires { n.out; }) {
                        if (value.contains(n.out) && variables[id(n.out)].contains != Contains::No &&
                            !std::is_same_v<N, ArrayAlloc>) {
                            unknown(id(n.out), StorageOrigin::Unknown, at);
                            for (const auto& [v, use] : operands) { (void)use; flow_id(n.out, v); }
                        }
                    }
                }, instructions[index]);
            }
        }
        for (const auto& [address, targets] : addresses) { (void)address; for (const auto v : targets) variables[v].facts.address_exposed = true; }
    }
    std::size_t root(std::size_t seed) {
        auto current = seed;
        while (seeds[current].parent != current) current = seeds[current].parent;
        while (seeds[seed].parent != seed) { const auto next = seeds[seed].parent; seeds[seed].parent = current; seed = next; }
        return current;
    }
    void solve() {
        std::deque<std::size_t> work;
        std::vector<bool> queued(variables.size(), true);
        for (std::size_t i = 0; i < variables.size(); ++i) work.push_back(i);
        while (!work.empty()) {
            const auto from = work.front(); work.pop_front(); queued[from] = false;
            for (const auto to : variables[from].outgoing) {
                bool changed = false;
                for (const auto s : variables[from].storage) changed |= variables[to].storage.insert(s).second;
                if (variables[to].storage_available && !variables[from].storage_available) { variables[to].storage_available = false; changed = true; }
                if (!variables[to].borrowed && variables[from].borrowed) { variables[to].borrowed = true; changed = true; }
                if (!variables[to].foreign && variables[from].foreign) { variables[to].foreign = true; changed = true; }
                // A single tensor is one of its incoming storage alternatives.
                // Join those roots immediately, so a phi followed by a long
                // view chain does not copy every pre-join alternative per value.
                if (variables[to].tensor && !variables[to].storage.empty()) {
                    auto first = root(*variables[to].storage.begin());
                    for (const auto s : variables[to].storage) {
                        const auto other = root(s);
                        if (first != other) {
                            seeds[std::max(first,other)].parent = std::min(first,other);
                            first = std::min(first,other);
                        }
                    }
                    const std::set<std::size_t> joined{first};
                    if (variables[to].storage != joined) { variables[to].storage = joined; changed = true; }
                }
                if (changed && !queued[to]) { queued[to] = true; work.push_back(to); }
            }
        }
        for (auto& v : variables) if (v.tensor && !v.storage.empty()) {
            const auto first = *v.storage.begin();
            for (const auto s : v.storage) { const auto a = root(first), b = root(s); if (a != b) seeds[std::max(a,b)].parent = std::min(a,b); }
        }
    }
    AliasInfo finish() {
        std::map<std::size_t, StorageClassId> ids;
        AliasInfo result; result.complete = complete;
        for (std::size_t s = 0; s < seeds.size(); ++s) {
            const auto r = root(s);
            if (!ids.contains(r)) {
                if (result.classes.size() >= std::numeric_limits<StorageClassId>::max()) throw std::length_error("alias class identity overflow");
                StorageClass c; c.id = static_cast<StorageClassId>(result.classes.size()); c.owned = true;
                ids.emplace(r, c.id); result.classes.push_back(std::move(c));
            }
        }
        const auto merge = [](StorageClass& into, const StorageClass& from, bool owner_seed) {
            if (owner_seed) into.owned &= from.owned;
            into.borrowed |= from.borrowed; into.may_have_other_owners |= from.may_have_other_owners;
            into.reaches_autograd |= from.reaches_autograd; into.address_exposed |= from.address_exposed;
            into.escapes_function |= from.escapes_function; into.alias_available &= from.alias_available;
            into.ownership_available &= from.ownership_available; into.state_identity_available &= from.state_identity_available;
            for (const auto origin : from.origins) append(into.origins, origin);
            for (const auto site : from.sites) append(into.sites, site);
            for (const auto& state : from.state) append(into.state, state);
        };
        for (std::size_t s = 0; s < seeds.size(); ++s) merge(result.classes[ids.at(root(s))], seeds[s].facts, true);
        for (const auto& v : variables) for (const auto s : v.storage) {
            auto& c = result.classes[ids.at(root(s))]; merge(c, v.facts, false); c.borrowed |= v.borrowed;
            if (v.foreign) c.owned = false;
            if (!v.storage_available) { c.alias_available = false; result.complete = false; }
        }
        for (auto& c : result.classes) {
            c.owned &= c.ownership_available;
            result.complete &= c.alias_available && c.ownership_available && c.state_identity_available;
            std::sort(c.origins.begin(), c.origins.end());
        }
        const auto classes = [&](const Variable& v) {
            std::vector<StorageClassId> out;
            for (const auto s : v.storage) append(out, ids.at(root(s)));
            std::sort(out.begin(), out.end()); return out;
        };
        for (const auto& [value_id, index] : value) {
            const auto& v = variables[index];
            StorageValue out{value_id, classes(v), v.storage_available, false, v.borrowed};
            out.owned = v.tensor && v.descriptor_owned && v.storage_available && v.ownership_available && !v.borrowed && !out.classes.empty();
            for (const auto c : out.classes) out.owned &= result.classes[c].owned && result.classes[c].ownership_available;
            result.complete &= out.storage_available; result.values.push_back(std::move(out));
        }
        const auto projection = [&](std::size_t index) {
            return StorageProjection{classes(variables[index]), variables[index].storage_available};
        };
        for (const auto& [name, index] : slot) if (!slots.promotable(name)) {
            result.slot_classes.emplace(name, classes(variables[index]));
            result.slot_storage.emplace(name, projection(index));
        }
        for (const auto& [index, variable] : parameter) result.parameter_storage.emplace(index, projection(variable));
        for (const auto& [index, variable] : phi) result.phi_storage.emplace(index, projection(variable));
        return result;
    }
};
} // namespace
const StorageValue* AliasInfo::find(ValueId id) const noexcept {
    const auto found = std::lower_bound(values.begin(), values.end(), id, [](const StorageValue& value, ValueId id) { return value.value < id; });
    return found != values.end() && found->value == id ? &*found : nullptr;
}
StorageProjection AliasInfo::storage_of(const ReachingDef& definition) const {
    if (!definition.available) return {};
    switch (definition.kind) {
    case ReachingDef::Undefined:
    case ReachingDef::Empty: return {{}, true};
    case ReachingDef::Store:
        if (const auto* value = find(definition.value)) return {value->classes, value->storage_available};
        return {};
    case ReachingDef::Param: {
        const auto found = parameter_storage.find(definition.parameter);
        return found == parameter_storage.end() ? StorageProjection{} : found->second;
    }
    case ReachingDef::Phi: {
        const auto found = phi_storage.find(definition.phi.value);
        return found == phi_storage.end() ? StorageProjection{} : found->second;
    }
    }
    return {};
}
bool AliasInfo::may_alias(ValueId a, ValueId b) const noexcept {
    const auto* left = find(a); const auto* right = find(b);
    if (!left || !right) return true;
    if ((left->storage_available && left->classes.empty()) || (right->storage_available && right->classes.empty())) return false;
    if (!left->storage_available || !right->storage_available) return true;
    for (const auto id : left->classes) if (id >= classes.size() || !classes[id].alias_available) return true;
    for (const auto id : right->classes) if (id >= classes.size() || !classes[id].alias_available) return true;
    for (const auto id : left->classes) if (std::binary_search(right->classes.begin(), right->classes.end(), id)) return true;
    return false;
}
AliasInfo alias_analysis(const Function& function, const DefUse& du, const LocalSsa& ssa, const Slots& slots, const Module* module) {
    Builder builder{function, du, ssa, slots, Types{module}, {}, {}, {}, {}, {}, {}, {}, {}, {}, true};
    builder.initialize(); builder.address_flows(); builder.instructions(); builder.solve(); return builder.finish();
}
} // namespace quidra::ir::analysis
