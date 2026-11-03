#include "ir/analysis/local_ssa.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
const auto integer = Type::simple(TypeKind::Int64);
const auto boolean = Type::simple(TypeKind::Bool);
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LocalSsa overlay(const Function& function) {
    const auto cfg = build_cfg(function);
    return build_local_ssa(function, cfg, dominators(cfg), classify_slots(function));
}

void slot_exclusions() {
    Function function;
    function.parameters = {{"owned", integer}, {"borrowed", integer, false, true},
        {"writable", integer, true, true}, {"copied_const", integer, false, false, true}};
    function.blocks = {{"entry", {DeclareLocal{"element", integer, {}, 0, 0}, DeclareLocal{"taken", integer, {}, 0, 0},
        DeclareReference{"reference", integer}, DeclareLocal{"moved", Type::simple(TypeKind::String), {}, 0, 0},
        ConstantInt{3, "0", integer}, AddressLocal{1, "borrowed"}, AddressLocal{2, "taken"}, BindReference{"reference", 2},
        StoreLocal{"writable", 3, integer, true}, LoadLocal{4, "moved", Type::simple(TypeKind::String), true}, ReturnVoid{}}},
        {"unreachable", {StoreLocal{"element", 3, integer, true}, ReturnVoid{}}}};
    const auto slots = classify_slots(function);
    require(slots.promotable("owned") && slots.promotable("copied_const"), "copied const annotation must not invent a borrow");
    require(!slots.promotable("borrowed") && !slots.promotable("writable") && !slots.promotable("element"), "borrowed/writable slots must never promote");
    require(slots.cls.at("element") == SlotClass::BorrowedLocal && slots.facts.at("moved").move_out, "borrowed element and move-out facts");
    require(!slots.promotable("taken") && !slots.promotable("reference") && slots.promotable("moved"), "exposed/reference slots stay memory while owned moves have explicit SSA clears");
    require(slots.facts.at("borrowed").borrowed_parameter && slots.facts.at("borrowed").address_taken,
        "representative class cannot erase overlapping borrow/exposure facts");
    require(slots.facts.at("writable").writable_parameter && slots.facts.at("writable").borrowed_parameter && slots.facts.at("writable").borrowed_local,
        "writable/borrowed parameter and borrowed store facts all survive");
}

void straight_and_sparse() {
    Function function;
    function.parameters = {{"parameter", integer}};
    constexpr auto sparse = std::numeric_limits<ValueId>::max();
    function.blocks = {{"entry", {DeclareLocal{"x", integer, {}, 0, 0}, LoadLocal{sparse, "x", integer},
        ConstantInt{17, "7", integer}, StoreLocal{"x", 17, integer}, LoadLocal{18, "x", integer}, LoadLocal{19, "parameter", integer}, ReturnVoid{}}},
        {"dead", {LoadLocal{20, "x", integer}, ReturnVoid{}}}};
    const auto ssa = overlay(function);
    require(ssa.phis.empty() && ssa.load_source.size() == 4, "sparse outputs use actual loads only");
    require(ssa.load_source.at(sparse).available && ssa.load_source.at(sparse).kind == ReachingDef::Undefined, "reachable undefined differs from unreachable");
    require(ssa.load_source.at(18).kind == ReachingDef::Store && ssa.load_source.at(18).value == 17 && ssa.load_source.at(18).store == InstrRef{0, 3}, "load reaches the actual preceding store");
    require(ssa.load_source.at(19).kind == ReachingDef::Param && ssa.load_source.at(19).parameter == 0, "parameter source is its typed parameter index");
    require(!ssa.load_source.at(20).available && !ssa.reaching({1, 0}, "x").available, "unreachable loads have no implicit dominance proof");
    require(ssa.reaching({0, 3}, "x").kind == ReachingDef::Undefined && ssa.reaching({0, 4}, "x").kind == ReachingDef::Store, "reaching state is before the queried instruction");
    require(!ssa.reaching({0, 99}, "x").available && !ssa.reaching({0, 0}, "absent").available, "invalid query domain is unavailable");
}

Function diamond(bool overwrite, bool missing) {
    Function function;
    function.parameters = {{"condition", boolean}};
    function.blocks = {{"entry", {DeclareLocal{"x", integer, {}, 0, 0}, DeclareLocal{"dead_value", integer, {}, 0, 0},
        LoadLocal{1, "condition", boolean}, Branch{1, "left", "right"}}},
        {"left", {ConstantInt{2, "13", integer}, StoreLocal{"x", 2, integer}, StoreLocal{"dead_value", 2, integer}, Jump{"join"}}},
        {"right", {ConstantInt{3, "29", integer}, StoreLocal{"dead_value", 3, integer}, Jump{"join"}}},
        {"join", {LoadLocal{4, "x", integer}, ReturnVoid{}}},
        {"unreachable", {ConstantInt{5, "99", integer}, StoreLocal{"x", 5, integer}, Jump{"join"}}}};
    if (!missing) function.blocks[2].instructions.insert(function.blocks[2].instructions.begin() + 1, StoreLocal{"x", 3, integer});
    if (overwrite) {
        function.blocks[3].instructions.insert(function.blocks[3].instructions.begin(), StoreLocal{"x", 7, integer});
        function.blocks[3].instructions.insert(function.blocks[3].instructions.begin(), ConstantInt{7, "31", integer});
    }
    return function;
}
void pruning_and_edges() {
    const auto function = diamond(false, false);
    const auto cfg = build_cfg(function);
    const auto ssa = overlay(function);
    require(ssa.phis.size() == 1 && ssa.phis[0].block == 3 && ssa.phis[0].local == "x", "live join gets one phi and dead local gets none");
    const auto& incoming = ssa.phis[0].incoming;
    require(incoming.size() == cfg.pred[3].size() && incoming[0].first == 1 && incoming[1].first == 2 && incoming[2].first == 4, "phi incoming preserves all authored CFG predecessors");
    require(incoming[0].second.value == 2 && incoming[1].second.value == 3 && !incoming[2].second.available, "unreachable incoming is retained without invented definition");
    const auto overwritten = overlay(diamond(true, false));
    require(overwritten.phis.empty(), "store before join load prunes the old dead phi");
    require(!overwritten.reaching({3, 1}, "x").available && overwritten.load_source.at(4).kind == ReachingDef::Store,
        "pruned ambiguous old state is unavailable rather than an invented dominating definition");
    const auto again = overlay(function);
    require(again.phis == ssa.phis && again.load_source == ssa.load_source, "phi identities and inputs are deterministic");
}

// Original execution uses only named local state. Overlay execution separately
// resolves Param/Store/Phi definitions, with phi values updated simultaneously
// on real incoming edges. No liveness, dominance or frontier logic is an oracle.
struct Empty { bool operator==(const Empty&) const = default; };
using Value = std::variant<std::monostate, std::int64_t, std::string, Empty>;
void interpret(const Function& function, const std::vector<std::int64_t>& arguments) {
    const auto ssa = overlay(function);
    const auto slots = classify_slots(function);
    const auto cfg = build_cfg(function);
    std::unordered_map<std::string, Value> locals;
    std::unordered_map<ValueId, Value> values;
    std::unordered_map<std::uint32_t, Value> phi_values;
    for (std::size_t i = 0; i < function.parameters.size(); ++i) locals[function.parameters[i].name] = arguments.at(i);
    const auto resolve = [&](const ReachingDef& definition) -> Value {
        require(definition.available, "executed overlay source is unavailable");
        switch (definition.kind) {
        case ReachingDef::Undefined: return std::monostate{};
        case ReachingDef::Param: return arguments.at(definition.parameter);
        case ReachingDef::Store: return values.at(definition.value);
        case ReachingDef::Phi: return phi_values.at(definition.phi.value);
        case ReachingDef::Empty: require(definition.empty_type.has_value(), "empty definition must retain its type"); return Empty{};
        }
        throw std::runtime_error("unknown reaching kind");
    };
    BlockId block = 0;
    std::optional<BlockId> predecessor;
    for (unsigned visits = 0; visits < 1000; ++visits) {
        std::vector<std::pair<std::uint32_t, Value>> simultaneous;
        for (const auto& phi : ssa.phis) if (phi.block == block) {
            if (!predecessor) { require(phi.entry.has_value(), "entry phi lacks external entry definition"); simultaneous.emplace_back(phi.id.value, resolve(*phi.entry)); }
            else {
                const auto incoming = std::find_if(phi.incoming.begin(), phi.incoming.end(), [&](const auto& edge) { return edge.first == *predecessor; });
                require(incoming != phi.incoming.end(), "actual predecessor missing from phi");
                simultaneous.emplace_back(phi.id.value, resolve(incoming->second));
            }
        }
        for (const auto& [id, value] : simultaneous) phi_values[id] = value;
        std::optional<BlockId> next;
        for (std::uint32_t index = 0; index < function.blocks[block].instructions.size(); ++index) {
            const auto& instruction = function.blocks[block].instructions[index];
            if (const auto* n = std::get_if<ConstantInt>(&instruction)) values[n->out] = std::stoll(n->value);
            else if (const auto* n = std::get_if<ConstantBool>(&instruction)) values[n->out] = std::int64_t(n->value);
            else if (const auto* n = std::get_if<ConstantString>(&instruction)) values[n->out] = n->value;
            else if (const auto* n = std::get_if<LoadLocal>(&instruction)) {
                const auto original = locals[n->name];
                if (slots.promotable(n->name)) {
                    require(resolve(ssa.load_source.at(n->out)) == original, "overlay load differs from original local interpreter");
                    require(resolve(ssa.reaching({block, index}, n->name)) == original, "before-instruction query differs from original state");
                }
                values[n->out] = original;
                if (n->move_out) locals[n->name] = Empty{};
            } else if (const auto* n = std::get_if<StoreLocal>(&instruction)) {
                if (slots.promotable(n->name)) require(resolve(ssa.reaching({block, index}, n->name)) == locals[n->name], "store query observes its own future write");
                locals[n->name] = values.at(n->value);
            } else if (const auto* n = std::get_if<Binary>(&instruction)) {
                const auto left = std::get<std::int64_t>(values.at(n->left)), right = std::get<std::int64_t>(values.at(n->right));
                if (n->op == "+") values[n->out] = left + right;
                else if (n->op == "<") values[n->out] = std::int64_t(left < right);
                else throw std::runtime_error("unsupported interpreter binary");
            } else if (const auto* n = std::get_if<Branch>(&instruction)) next = cfg.label.at(std::get<std::int64_t>(values.at(n->condition)) ? n->if_true : n->if_false);
            else if (const auto* n = std::get_if<Jump>(&instruction)) next = cfg.label.at(n->target);
            else if (std::holds_alternative<ReturnVoid>(instruction)) return;
        }
        require(next.has_value(), "interpreter block has no explicit transition");
        predecessor = block; block = *next;
    }
    throw std::runtime_error("interpreter loop exceeded bounded fixture");
}
Function loop(bool root_backedge) {
    Function function;
    function.parameters = {{"limit", integer}, {"x", integer}};
    function.blocks = {{"entry", {Jump{"header"}}},
        {"header", {LoadLocal{1, "x", integer}, LoadLocal{2, "limit", integer}, Binary{3, "<", 1, 2, integer, boolean}, Branch{3, "body", "exit"}}},
        {"body", {ConstantInt{4, "1", integer}, Binary{5, "+", 1, 4, integer, integer}, StoreLocal{"x", 5, integer}, Jump{"header"}}},
        {"exit", {LoadLocal{6, "x", integer}, ReturnVoid{}}}};
    if (root_backedge) function.blocks.erase(function.blocks.begin());
    return function;
}
void interpreter_oracles() {
    for (const bool missing : {false, true}) for (const bool condition : {false, true}) interpret(diamond(false, missing), {condition});
    for (const bool root_backedge : {false, true}) {
        const auto function = loop(root_backedge);
        const auto ssa = overlay(function);
        require(ssa.phis.size() == 1 && ssa.phis[0].local == "x", "only loop-carried live local requires phi");
        if (root_backedge) require(ssa.phis[0].entry && ssa.phis[0].entry->kind == ReachingDef::Param, "root backedge retains independent parameter entry");
        for (const auto limit : {0, 1, 9}) interpret(function, {limit, 0});
    }
}
void moved_values() {
    const auto string = Type::simple(TypeKind::String);
    Function function;
    function.parameters = {{"condition", boolean}};
    function.blocks = {{"entry", {DeclareLocal{"x", string, {}, 0, 0}, ConstantString{1, "owned"}, StoreLocal{"x", 1, string},
        LoadLocal{2, "condition", boolean}, Branch{2, "move", "keep"}}},
        {"move", {LoadLocal{3, "x", string, true}, LoadLocal{4, "x", string}, Jump{"join"}}},
        {"keep", {Jump{"join"}}}, {"join", {LoadLocal{5, "x", string}, ReturnVoid{}}}};
    const auto ssa = overlay(function);
    require(ssa.load_source.at(3).kind == ReachingDef::Store && ssa.load_source.at(4).kind == ReachingDef::Empty,
        "move reads the previous value then later loads see a separate empty definition");
    require(ssa.load_source.at(4).store == InstrRef{1, 0} && ssa.load_source.at(4).empty_type == string,
        "empty state retains its actual move site and type");
    require(ssa.phis.size() == 1 && ssa.phis[0].incoming[0].second.kind == ReachingDef::Empty &&
        ssa.phis[0].incoming[1].second.kind == ReachingDef::Store, "join differentiates emptied and retained owned state");
    interpret(function, {0}); interpret(function, {1});
    Function repeated;
    repeated.parameters = {{"limit", integer}};
    repeated.blocks = {{"entry", {DeclareLocal{"x", string, {}, 0, 0}, DeclareLocal{"i", integer, {}, 0, 0}, ConstantString{1, "initial"},
        StoreLocal{"x", 1, string}, ConstantInt{2, "0", integer}, StoreLocal{"i", 2, integer}, Jump{"header"}}},
        {"header", {LoadLocal{3, "x", string}, LoadLocal{4, "i", integer}, LoadLocal{5, "limit", integer},
            Binary{6, "<", 4, 5, integer, boolean}, Branch{6, "body", "exit"}}},
        {"body", {LoadLocal{7, "x", string, true}, ConstantInt{8, "1", integer}, Binary{9, "+", 4, 8, integer, integer},
            StoreLocal{"i", 9, integer}, Jump{"header"}}}, {"exit", {ReturnVoid{}}}};
    for (const auto limit : {0, 1, 3}) interpret(repeated, {limit});
}
void implicit_storage_and_finite_intrinsics() {
    const auto real = Type::simple(TypeKind::Real32);
    const auto string = Type::simple(TypeKind::String);
    const CheckedReal checked{2, real, {1, 1}, FiniteCause::addition, 2, 2};
    Function function;
    function.blocks = {{"entry", {ConstantBool{1, false}, StoreLocal{"flag", 1, boolean},
        ConstantFloat{2, 1.0, real, {}}, FiniteFlagUpdate{"flag", checked}, FiniteFlagCheck{"flag", checked},
        LoadLocal{3, "flag", boolean}, ConstantString{4, "owned"}, StoreLocal{"owned", 4, string},
        LoadLocal{5, "owned", string}, ReturnVoid{}}}};
    const auto slots = classify_slots(function);
    require(!slots.promotable("flag") && !slots.facts.at("flag").address_taken && !slots.facts.at("flag").reference_bound,
        "direct finite-flag effects are not ordinary loads or invented address exposure");
    require(slots.promotable("owned"), "implicit owned storage and terminal cleanup must not disable promotion");
    const auto ssa = overlay(function);
    require(!ssa.load_source.contains(3) && !ssa.reaching({0, 5}, "flag").available,
        "opaque finite flag cannot acquire an ordinary local SSA proof");
    require(ssa.load_source.at(5).kind == ReachingDef::Store && ssa.load_source.at(5).value == 4 &&
        ssa.reaching({0, 9}, "owned").kind == ReachingDef::Store,
        "implicit StoreLocal is the owned load and pre-return source");
}
void inline_storage_lifetimes() {
    const auto text=Type::simple(TypeKind::String);
    Function fn;InlineScope scope;scope.identity=9;scope.blocks={"entry"};
    scope.slots={{"owned",text,InlineSlotKind::OwnedLocal},{"borrowed",text,InlineSlotKind::BorrowedLocal},
                 {"raw",integer,InlineSlotKind::RawAddress},{"ref",text,InlineSlotKind::PinnedReference}};
    fn.inline_scopes={scope};fn.parameters={{"address_target",integer,true,true}};
    fn.blocks={{"entry",{ConstantInt{1,"4",integer},StoreLocal{"caller",1,integer},InlineScopeBegin{9},
        LoadLocal{2,"owned",text},ConstantString{3,"value"},StoreLocal{"owned",3,text},LoadLocal{4,"owned",text},
        AddressLocal{7,"address_target"},InlineBindAddress{"raw",7,integer},LoadLocal{8,"raw",integer},
        InlineScopeCleanup{9},LoadLocal{5,"owned",text},InlineScopeEnd{9},InlineScopeBegin{9},LoadLocal{6,"owned",text},
        InlineScopeCleanup{9},InlineScopeEnd{9},ReturnVoid{}}}};
    const auto slots=classify_slots(fn);const auto ssa=overlay(fn);
    require(slots.promotable("owned") && !slots.promotable("borrowed") && !slots.promotable("raw") && !slots.promotable("ref"),
        "typed inline slots preserve their exact owned/borrowed/raw/reference ABI");
    const auto& raw=slots.facts.at("raw");
    require(raw.writable_parameter && raw.borrowed_parameter && raw.address_taken && !ssa.load_source.contains(8),
        "raw address binding is borrowed writable memory rather than a promoted owned slot");
    require(ssa.load_source.at(2).kind==ReachingDef::Empty && ssa.load_source.at(4).value==3 &&
        ssa.load_source.at(5).kind==ReachingDef::Undefined && ssa.load_source.at(6).kind==ReachingDef::Empty,
        "begin initializes managed cells, cleanup destroys them, and repeated begin has fresh empty state");
    require(slots.promotable("caller") && !slots.promotable("address_target"),
        "caller preservation uses an unexposed owned cell distinct from the raw address target");
    require(ssa.reaching({0,10},"caller").value==1 && ssa.reaching({0,16},"caller").value==1,
        "scope cleanup cannot clear caller-owned storage");
}

}
int main() {
    try { inline_storage_lifetimes(); implicit_storage_and_finite_intrinsics(); slot_exclusions(); straight_and_sparse(); pruning_and_edges(); interpreter_oracles(); moved_values(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    std::puts("IR local SSA: slot safety, pruned phis and independent interpreter verified");
}
