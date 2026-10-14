#include "ir/analysis/alias.hpp"
#include "quidra/ir/module.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
const auto integer = Type::simple(TypeKind::Int64);
const auto boolean = Type::simple(TypeKind::Bool);
const auto tensor = Type::tensor(Type::simple(TypeKind::Real32), 1);
const auto shape = Type::array(integer);

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
AliasInfo analyze(const Function& function, const Module* module = nullptr) {
    const auto cfg = build_cfg(function);
    const auto slots = classify_slots(function);
    const auto ssa = build_local_ssa(function, cfg, dominators(cfg), slots);
    return alias_analysis(function, def_use(function, module), ssa, slots, module);
}
const StorageValue& value(const AliasInfo& aliases, ValueId id) {
    const auto* found = aliases.find(id);
    require(found != nullptr, "missing actual storage value");
    return *found;
}
const StorageClass& storage(const AliasInfo& aliases, StorageClassId id) {
    const auto found = std::find_if(aliases.classes.begin(), aliases.classes.end(),
        [id](const auto& item) { return item.id == id; });
    require(found != aliases.classes.end(), "missing actual storage class");
    return *found;
}
void borrowed(const AliasInfo& aliases, ValueId id, const char* message) {
    const auto& item = value(aliases, id);
    require(item.borrowed && !item.owned, message);
}

// A fresh descriptor must not be mistaken for fresh payload; sparse IDs must
// not turn a handful of values into an allocation indexed by UINT32_MAX.
void sparse_views_and_fresh_cast() {
    constexpr auto sparse = std::numeric_limits<ValueId>::max();
    const auto fallible = Type::union_of({tensor, Type::simple(TypeKind::Error)});
    Function function;
    function.blocks = {{"entry", {
        ConstantInt{1, "0", integer}, ConstantInt{2, "2", integer},
        ArrayMake{3, {2}, shape}, TensorCreate{4, 3, {}, tensor, tensor_fill_mode::zeros},
        Clone{sparse, 4, tensor}, TensorReshape{5, sparse, 3, tensor},
        TensorTranspose{6, 5, 1, 1, tensor}, TensorIndex{7, 6, {}, tensor},
        TensorContiguous{8, 7, tensor}, TensorTransfer{9, 8, {}, tensor},
        TensorCast{10, 4, tensor, tensor, tensor},
        TensorCast{11, 4, tensor, tensor, fallible}, ReturnVoid{}}}};
    const auto aliases = analyze(function);
    require(aliases.values.size() <= 12 && aliases.classes.size() <= 3,
        "sparse maximum ID must use bounded actual-value storage");
    require(std::is_sorted(aliases.values.begin(), aliases.values.end(),
        [](const auto& left, const auto& right) { return left.value < right.value; }),
        "actual sparse values must have deterministic order");
    for (const auto id : std::vector<ValueId>{sparse, 5, 6, 7, 8, 9})
        require(aliases.may_alias(4, id), "clone/view/contiguous/generic transfer must retain sharing");
    require(!aliases.may_alias(4, 10), "current same-dtype TensorCast allocates a fresh payload");
    require(!value(aliases, 11).owned && !aliases.may_alias(4, 11),
        "fallible TensorCast carrier contains fresh storage but is not a direct owned tensor donor");
    require(aliases.may_alias(4, 123456), "missing values must never certify disjoint storage");
}

// Borrowed values remain ineligible as owned donors even when their container
// or local happens to be managed and has a known tensor leaf type.
void borrowed_state_sources() {
    const auto holder = Type::class_type("Holder");
    const auto elements = Type::array(tensor);
    Module module;
    ClassLayout layout;
    layout.name = "Holder"; layout.field_names = {"item"}; layout.fields = {tensor};
    module.classes.push_back(layout);
    Function function;
    function.parameters = {{"input", tensor, false, true},
        {"holder", holder, false, true}, {"elements", elements, false, true}};
    function.blocks = {{"entry", {
        DeclareLocal{"view", tensor, {}, 0, 0}, DeclareReference{"reference", tensor},
        ConstantInt{1, "0", integer}, LoadLocal{2, "input", tensor},
        StoreLocal{"view", 2, tensor, true}, LoadLocal{3, "view", tensor},
        LoadLocal{4, "holder", holder}, FieldGet{5, 4, 0, tensor},
        LoadLocal{6, "elements", elements}, ArrayGet{7, 6, 1, tensor},
        AddressLocal{8, "input"}, BindReference{"reference", 8},
        LoadReference{9, "reference", tensor}, ReturnVoid{}}}};
    const auto aliases = analyze(function, &module);
    borrowed(aliases, 2, "borrowed parameter must not become an owned donor");
    borrowed(aliases, 3, "borrowed local must not become an owned donor");
    borrowed(aliases, 5, "borrowed field must not become an owned donor");
    borrowed(aliases, 7, "borrowed element must not become an owned donor");
    borrowed(aliases, 9, "reference load must not become an owned donor");
    require(aliases.may_alias(2, 9), "bound reference must retain possible sharing with its target");
    for (const auto id : value(aliases, 9).classes)
        require(!storage(aliases, id).state_identity_available,
            "relative reference binding must not invent persistent target identity");
    const auto missing_layout = analyze(function);
    require(!value(missing_layout, 5).storage_available && missing_layout.may_alias(2, 5),
        "missing class layout must not invent disjoint field storage");
}

// Internal projections borrow a function-owned object's leaf. This differs
// from introducing an external borrowed owner of its backing allocation.
void internal_projection_preserves_root_ownership() {
    const auto holder = Type::class_type("Holder");
    Module module;
    ClassLayout layout;
    layout.name = "Holder"; layout.field_names = {"item"}; layout.fields = {tensor};
    module.classes.push_back(layout);
    Function function;
    function.blocks = {{"entry", {
        ConstantInt{1, "2", integer}, ArrayMake{2, {1}, shape},
        TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros},
        ClassMake{4, holder, {3}}, FieldGet{5, 4, 0, tensor},
        TensorReshape{6, 5, 2, tensor}, ReturnVoid{}}}};
    const auto aliases = analyze(function, &module);
    borrowed(aliases, 5, "internal field projection is borrowed as a value");
    borrowed(aliases, 6, "view of a borrowed field retains borrowed value provenance");
    require(aliases.may_alias(3, 6), "internal view must retain its owned backing root");
    require(!value(aliases, 3).classes.empty(), "fresh tensor needs a backing class");
    for (const auto id : value(aliases, 3).classes)
        require(storage(aliases, id).owned && storage(aliases, id).ownership_available,
            "internal borrowed projection must not veto function ownership of a fresh root");
}

void promotable_move_out() {
    Function function;
    function.blocks = {{"entry", {
        DeclareLocal{"owned", tensor, {}, 0, 0}, ConstantInt{1, "2", integer},
        ArrayMake{2, {1}, shape}, TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros},
        StoreLocal{"owned", 3, tensor}, LoadLocal{4, "owned", tensor},
        LoadLocal{5, "owned", tensor, true}, ReturnVoid{}}}};
    const auto slots = classify_slots(function);
    require(slots.promotable("owned"), "an unexposed owned move slot remains promotable");
    const auto aliases = analyze(function);
    borrowed(aliases, 4, "ordinary owned-local read is still a borrowed value");
    require(value(aliases, 5).owned && !value(aliases, 5).borrowed,
        "known promotable move-out must retain owned value provenance");
    require(aliases.may_alias(3, 5), "move-out transfers ownership without copying payload");
}

void local_phi_keeps_both_roots() {
    Function function;
    function.parameters = {{"condition", boolean}};
    function.blocks = {
        {"entry", {DeclareLocal{"choice", tensor, {}, 0, 0},
            LoadLocal{1, "condition", boolean}, ConstantInt{2, "2", integer},
            ArrayMake{3, {2}, shape}, Branch{1, "left", "right"}}},
        {"left", {TensorCreate{4, 3, {}, tensor, tensor_fill_mode::zeros},
            StoreLocal{"choice", 4, tensor}, Jump{"join"}}},
        {"right", {TensorCreate{5, 3, {}, tensor, tensor_fill_mode::zeros},
            StoreLocal{"choice", 5, tensor}, Jump{"join"}}},
        {"join", {LoadLocal{6, "choice", tensor}, ReturnVoid{}}}};
    const auto aliases = analyze(function);
    require(aliases.may_alias(6, 4) && aliases.may_alias(6, 5),
        "local SSA phi must retain storage from both incoming branches");
    borrowed(aliases, 6, "phi local read is borrowed rather than an owning donation");
}

void nested_containers_retain_sources() {
    const auto pair = Type::tuple({tensor, tensor});
    const auto array = Type::array(pair);
    const auto box = Type::class_type("Box");
    Module module;
    ClassLayout layout;
    layout.name = "Box"; layout.field_names = {"items"}; layout.fields = {array};
    module.classes.push_back(layout);
    Function function;
    function.blocks = {{"entry", {
        ConstantInt{1, "2", integer}, ArrayMake{2, {1}, shape},
        TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros}, Clone{4, 3, tensor},
        TensorCreate{5, 2, {}, tensor, tensor_fill_mode::zeros},
        TupleMake{6, {4, 5}, pair}, ArrayMake{7, {6}, array}, ClassMake{8, box, {7}},
        FieldGet{9, 8, 0, array}, ConstantInt{10, "0", integer},
        ArrayGet{11, 9, 10, pair}, TupleGet{12, 11, 0, tensor},
        TupleGet{13, 11, 1, tensor}, ReturnVoid{}}}};
    const auto aliases = analyze(function, &module);
    require(aliases.may_alias(3, 8) && aliases.may_alias(5, 8),
        "nested class/array/tuple must retain both contained tensor sources");
    require(aliases.may_alias(3, 12) && aliases.may_alias(5, 13),
        "nested projections must not invent fresh tensor payloads");
    borrowed(aliases, 12, "nested managed projection must remain borrowed");
    borrowed(aliases, 13, "second nested managed projection must remain borrowed");
}

void opaque_call_is_not_a_freshness_proof() {
    Function function;
    function.blocks = {{"entry", {
        ConstantInt{1, "2", integer}, ArrayMake{2, {1}, shape},
        TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros},
        Call{4, "opaque", {{3, {}}}, tensor}, ReturnVoid{}}}};
    const auto aliases = analyze(function);
    require(!value(aliases, 4).storage_available && !value(aliases, 4).owned,
        "opaque call result must retain unavailable provenance rather than fresh owned storage");
    require(aliases.may_alias(3, 4) && !aliases.complete,
        "missing call summary must be conservative and explicitly incomplete");
    require(!value(aliases, 3).storage_available,
        "opaque argument mutation must not leave a closed pre-call storage set");
    for (const auto id : value(aliases, 3).classes)
        require(storage(aliases, id).escapes_function && !storage(aliases, id).alias_available,
            "opaque call arguments may escape even when the caller originally owned them");
}

void inline_raw_alias_flow() {
    Function fn;InlineScope scope;scope.identity=7;scope.blocks={"entry"};scope.slots={{"raw",tensor,InlineSlotKind::RawAddress}};fn.inline_scopes={scope};
    fn.blocks={{"entry",{ConstantInt{1,"2",integer},ArrayMake{2,{1},shape},TensorCreate{3,2,{},tensor,tensor_fill_mode::zeros},
        StoreLocal{"caller",3,tensor},TensorCreate{4,2,{},tensor,tensor_fill_mode::zeros},InlineScopeBegin{7},
        AddressLocal{5,"caller"},InlineBindAddress{"raw",5,tensor},StoreLocal{"raw",4,tensor,true},
        LoadLocal{6,"caller",tensor},LoadLocal{7,"raw",tensor},InlineScopeCleanup{7},InlineScopeEnd{7},ReturnVoid{}}}};
    const auto a=analyze(fn);
    require(a.may_alias(4,6) && a.may_alias(3,7) && a.may_alias(4,7),"raw inline rebinding reaches the actual caller address and its alias set");
    borrowed(a,7,"raw inline load cannot receive descriptor ownership");
}
} // namespace

int main() {
    try {
        inline_raw_alias_flow();
        sparse_views_and_fresh_cast();
        borrowed_state_sources();
        internal_projection_preserves_root_ownership();
        promotable_move_out();
        local_phi_keeps_both_roots();
        nested_containers_retain_sources();
        opaque_call_is_not_a_freshness_proof();
        std::puts("storage alias analysis: seven focused fixtures passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "storage alias analysis: %s\n", error.what());
        return 1;
    }
}
