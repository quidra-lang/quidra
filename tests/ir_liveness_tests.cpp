#include "ir/analysis/liveness.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> bool contains(const std::vector<T>& values, const T& value) {
    return std::binary_search(values.begin(), values.end(), value);
}
const auto integer = Type::simple(TypeKind::Int64);
const auto boolean = Type::simple(TypeKind::Bool);

void diamond_loop_and_sparse_values() {
    constexpr ValueId high = std::numeric_limits<ValueId>::max();
    Function function;
    function.parameters = {{"condition", boolean}};
    function.blocks = {
        {"entry", {ConstantInt{high, "9", integer}, LoadLocal{1, "condition", boolean}, Branch{1, "loop", "error"}}},
        {"loop", {LoadLocal{2, "condition", boolean}, Branch{2, "loop", "ok"}}},
        {"ok", {Return{high, integer}}},
        {"error", {ConstantString{3, "failure"}, FailError{3}}}};
    const auto live = liveness(function, build_cfg(function), def_use(function), classify_slots(function), {});
    require(contains(live.blocks[0].out.values, high) && contains(live.blocks[1].in.values, high),
            "value used beyond a backedge is live, including sparse maximum ID");
    require(!contains(live.blocks[0].in.values, high), "definition kills its incoming SSA demand");
    require(contains(live.blocks[0].in.locals, std::string("condition")), "loop local demand reaches entry");
    require(contains(live.before({3, 1})->values, ValueId{3}), "explicit error operand is live");
    require(live.after({3, 1})->values.empty(), "failure exit has no imaginary SSA successor");
}

void exceptional_continuation_and_local_replacement() {
    Function function;
    const auto string = Type::simple(TypeKind::String);
    function.parameters = {{"out", integer, true}};
    function.blocks = {
        {"entry", {ConstantInt{1, "4", integer}, StoreLocal{"out", 1, integer},
            ConstantString{2, "old"}, StoreLocal{"text", 2, string},
            ConstantString{3, "new"}, StoreLocal{"text", 3, string},
            ConstantInt{4, "7", integer}, ReturnVoid{}}},
        {"handler", {Return{1, integer}}}};
    LivenessOptions options; options.failure_edges.push_back({{0, 6}, 1});
    const auto live = liveness(function, build_cfg(function), def_use(function), classify_slots(function), {}, options);
    require(contains(live.before({0, 6})->values, ValueId{1}) && !contains(live.after({0, 6})->values, ValueId{1}),
            "exceptional demand is before the instruction, not normal continuation");
    require(contains(live.before({0, 5})->locals, std::string("text")), "replacement reads the old managed local");
    require(contains(live.blocks[0].out.locals, std::string("out")), "writable parameter remains observable at exit");
    require(contains(live.blocks[0].out.locals, std::string("text")), "implicit exit cleanup retains owned local");
    require(!live.blocks[1].reachable, "exceptional-only block does not invent entry-CFG reachability");
    require(live.before({99, 0}) == nullptr && live.after({0, 99}) == nullptr, "invalid query has no proof");
}

void storage_views_and_unknown_provenance() {
    Function function;
    function.blocks = {{"entry", {ConstantInt{1, "1", integer}, ConstantInt{2, "2", integer},
        Binary{3, "+", 1, 2, integer, integer}, Return{3, integer}}}};
    AliasInfo aliases;
    aliases.values = {{1, {4}, true, true, false}, {2, {4}, true, false, true}, {3, {}, false, false, false}};
    StorageClass shared; shared.id = 4; shared.owned = true;
    StorageClass external; external.id = 8; external.borrowed = true;
    aliases.classes = {shared, external};
    const auto live = liveness(function, build_cfg(function), def_use(function), classify_slots(function), aliases);
    require(contains(live.before({0, 2})->storage, StorageClassId{4}), "all live views retain shared storage");
    require(contains(live.blocks[0].out.storage, StorageClassId{8}), "borrowed external storage is retained conservatively");
    require(!live.before({0, 3})->storage_available && contains(live.before({0, 3})->storage, StorageClassId{4}),
            "unknown provenance cannot certify a storage class dead");
    require(!live.storage_complete, "unknown storage remains explicit");

    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32), 1);
    const auto shape = Type::array(integer);
    Function locals;
    locals.blocks = {{"entry", {ConstantInt{1, "1", integer}, ArrayMake{2, {1}, shape},
        TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros}, StoreLocal{"x", 3, tensor},
        TensorCreate{4, 2, {}, tensor, tensor_fill_mode::ones}, StoreLocal{"x", 4, tensor},
        LoadLocal{5, "x", tensor, true}, Release{5, tensor}, Release{2, shape}, ReturnVoid{}}}};
    const auto cfg = build_cfg(locals);
    const auto slots = classify_slots(locals);
    const auto ssa = build_local_ssa(locals, cfg, dominators(cfg), slots);
    AliasInfo known;
    known.values = {{1, {}, true, false, false}, {2, {}, true, false, false},
        {3, {11}, true, true, false}, {4, {12}, true, true, false}, {5, {12}, true, true, false}};
    StorageClass old; old.id = 11; old.owned = true;
    StorageClass next; next.id = 12; next.owned = true;
    known.classes = {old, next};
    const auto precise = liveness(locals, cfg, def_use(locals), slots, known, {}, &ssa);
    require(precise.after({0, 3})->storage == std::vector<StorageClassId>{11},
            "promotable local retains only its actual reaching storage");
    require(precise.before({0, 5})->storage == std::vector<StorageClassId>({11, 12}) &&
            precise.after({0, 5})->storage == std::vector<StorageClassId>{12},
            "replacement keeps old storage through release and then switches to new storage");
    require(precise.after({0, 6})->storage == std::vector<StorageClassId>{12} &&
            precise.after({0, 7})->storage.empty() && precise.storage_complete,
            "move clears the local while its SSA owner keeps storage live through release");
}

void inline_cleanup_roots() {
    const auto text=Type::simple(TypeKind::String);Function fn;InlineScope scope;scope.identity=5;scope.blocks={"entry"};
    scope.slots={{"owned",text,InlineSlotKind::OwnedLocal},{"borrowed",text,InlineSlotKind::BorrowedLocal}};fn.inline_scopes={scope};
    fn.blocks={{"entry",{ConstantString{1,"outer"},StoreLocal{"caller",1,text},InlineScopeBegin{5},ConstantString{2,"inner"},
        StoreLocal{"owned",2,text},StoreLocal{"borrowed",2,text,true},InlineScopeCleanup{5},InlineScopeEnd{5},
        ConstantInt{3,"0",integer},InlineExit{5,3}}}};
    LivenessOptions options;options.conservative_failure_cleanup=false;
    const auto live=liveness(fn,build_cfg(fn),def_use(fn),classify_slots(fn),{},options);
    require(live.before({0,6})->locals==std::vector<std::string>{"owned"} && live.after({0,6})->locals.empty(),
        "scope cleanup reads and destroys only its owned cells, not borrowed or caller storage");
    require(live.blocks[0].out.locals.empty(),"InlineExit cannot introduce function/caller cleanup roots");
}
} // namespace

int main() {
    try {
        inline_cleanup_roots();
        diamond_loop_and_sparse_values();
        exceptional_continuation_and_local_replacement();
        storage_views_and_unknown_provenance();
        std::cout << "IR liveness tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
