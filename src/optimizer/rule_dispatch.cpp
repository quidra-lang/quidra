#include "optimizer/rule_dispatch.hpp"
#include "optimizer/call_contract.hpp"
#include "optimizer/execution_policy.hpp"
#include "optimizer/function_index.hpp"
#include "optimizer/static_tensor_facts.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "ir/function_builder.hpp"
#include "platform/sha256.hpp"
#include "quidra/native_extension.h"

#include <algorithm>
#include <map>
#include <limits>
#include <stdexcept>
#include <set>
#include <tuple>
#include <utility>

namespace quidra::optimizer {
namespace {
using namespace ir;
struct Guard {
    enum class Kind : std::uint8_t { Backend, Device, Tracked, Contiguous, Rank, Extent, SameShape } kind;
    std::size_t parameter{}, other{};
    std::int64_t expected{};
    bool operator==(const Guard&) const = default;
};
struct Candidate { const Function* target{}; const ConditionalRule* rule{}; std::vector<Guard> guards; };
struct Rewrite { std::size_t function{}, block{}, instruction{}; std::string callee; };
bool compatible(const Function& source, const Function& target, const Type& result, RuleStage stage) {
    if (source.parameters.size() != target.parameters.size() || !replacement_result_compatible(target.result, result)) return false;
    for (std::size_t i = 0; i < source.parameters.size(); ++i) {
        const auto& a = source.parameters[i]; const auto& b = target.parameters[i];
        if (a.type != b.type || a.writable != b.writable || (a.is_const && !b.is_const)) return false;
        // Changing consuming/borrowing is an inferred ABI difference. A
        // memory rule may never turn a borrowed input into a consumed donor
        // without a caller ownership transfer proof.
        if (stage == RuleStage::MemoryReuse && a.borrowed && !b.borrowed) return false;
    }
    return true;
}
bool same_abi(const Function& source, const Function& target) {
    for (std::size_t i = 0; i < source.parameters.size(); ++i)
        if (!same_parameter_contract(target.parameters[i], source.parameters[i])) return false;
    return true;
}
std::optional<std::int64_t> backend_code(const std::string& value) {
    if (value == "cpu") return QCORE_BACKEND_CPU;
    if (value == "cuda") return QCORE_BACKEND_CUDA;
    if (value == "hip") return QCORE_BACKEND_HIP;
    if (value == "metal") return QCORE_BACKEND_METAL;
    if (value == "test") return QCORE_BACKEND_TEST;
    return {};
}
struct GuardPlan { enum class Status { Ready, Contradicted, Unavailable } status{Status::Ready}; std::vector<Guard> guards; std::string reason; };
GuardPlan guards_for(const ConditionalRule& rule, const Function& source, const Call& call,
    std::size_t tensor, const StaticTensorFacts& fact,
    const std::unordered_map<std::string, std::string>& policies,
    const analysis::AliasInfo* aliases, const analysis::Liveness* liveness, analysis::InstrRef at) {
    GuardPlan result;
    const auto contradiction = [&] { result.status = GuardPlan::Status::Contradicted; };
    const auto unavailable = [&](std::string reason) { if (result.status != GuardPlan::Status::Contradicted) { result.status = GuardPlan::Status::Unavailable; result.reason = std::move(reason); } };
    const auto& c = rule.constraints;
    if (c.dtype) {
        if (!fact.has_type || !fact.type.first) unavailable("dtype fact unavailable");
        else if (type_name(*fact.type.first) != *c.dtype) contradiction();
    }
    if (c.rank) result.guards.push_back({Guard::Kind::Rank, tensor, 0, *c.rank});
    if (c.shape_present || !c.shape.empty()) {
        result.guards.push_back({Guard::Kind::Rank, tensor, 0, static_cast<std::int64_t>(c.shape.size())});
        for (std::size_t axis = 0; axis < c.shape.size(); ++axis)
            result.guards.push_back({Guard::Kind::Extent, tensor, axis, c.shape[axis]});
    }
    if (c.backend) {
        const auto backend = backend_code(*c.backend);
        if (!backend) unavailable("backend enum unavailable");
        else result.guards.push_back({Guard::Kind::Backend, tensor, 0, *backend});
    }
    if (c.device) result.guards.push_back({Guard::Kind::Device, tensor, 0, *c.device == StaticDevice::Gpu ? 1 : 0});
    if (c.contiguous) result.guards.push_back({Guard::Kind::Contiguous, tensor, 0, *c.contiguous});
    if (c.tracked) result.guards.push_back({Guard::Kind::Tracked, tensor, 0, *c.tracked});
    if (c.policy) {
        const auto found = policies.find(rule.extension);
        if (found == policies.end()) unavailable("runtime policy query contract unavailable");
        else if (found->second != *c.policy) contradiction();
    }
    if (c.owned) {
        const auto* value = aliases && tensor < call.args.size() ? aliases->find(call.args[tensor].value) : nullptr;
        if (!value || !value->storage_available || value->classes.empty()) unavailable("owned provenance unavailable; runtime uniqueness cannot prove ownership");
        else {
            bool available = true;
            for (const auto id : value->classes) {
                const auto found = std::find_if(aliases->classes.begin(), aliases->classes.end(), [&](const auto& storage) { return storage.id == id; });
                if (found == aliases->classes.end() || !found->ownership_available || !found->alias_available) available = false;
            }
            if (!available) unavailable("owned provenance incomplete");
            else if ((value->owned && !value->borrowed) != *c.owned) contradiction();
        }
    }
    if (c.last_use) {
        const auto* value = aliases && tensor < call.args.size() ? aliases->find(call.args[tensor].value) : nullptr;
        const auto* live = liveness ? liveness->after(at) : nullptr;
        if (!value || !value->storage_available || !live || !live->storage_available || !liveness->storage_complete || !liveness->roles_complete)
            unavailable("storage liveness unavailable or incomplete");
        else {
            bool last = true;
            for (const auto id : value->classes)
                if (std::binary_search(live->storage.begin(), live->storage.end(), id)) last = false;
            if (last != *c.last_use) contradiction();
        }
    }
    if (!c.same_shape.empty()) {
        std::optional<std::size_t> first;
        for (const auto& name : c.same_shape) {
            const auto found = std::find_if(source.parameters.begin(), source.parameters.end(), [&](const auto& p) { return p.name == name; });
            if (found == source.parameters.end() || found->type.kind != TypeKind::Tensor) { unavailable("same_shape parameter unavailable"); break; }
            const auto index = static_cast<std::size_t>(found - source.parameters.begin());
            if (!first) { first = index; continue; }
            result.guards.push_back({Guard::Kind::SameShape, *first, index, 0});
        }
    }
    return result;
}

class StubBuilder {
public:
    StubBuilder(const Function& source, Type result, std::string name, std::vector<Function>& queries)
        : source_(source), queries_(queries) {
        function_.name = std::move(name); function_.source_file = source.source_file;
        function_.source_line = source.source_line; function_.source_column = source.source_column;
        function_.parameters = source.parameters; function_.result = std::move(result);
        function_.dispatch_stub = true;
        builder_ = FunctionBuilder(function_, 1, 0, 0); block("entry");
    }
    Function build(const std::vector<Candidate>& candidates) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto next = "rule." + std::to_string(i + 1);
            for (const auto& guard : candidates[i].guards) emit_guard(guard, next);
            forward(*candidates[i].target);
            block(next);
        }
        forward(source_); builder_.finish(); return std::move(function_);
    }
private:
    const Function& source_;
    std::vector<Function>& queries_;
    Function function_;
    FunctionBuilder builder_;
    std::size_t label_{};
    void block(const std::string& label) { builder_.enter(builder_.add_block(label)); }
    std::string label(std::string_view prefix) { return std::string(prefix) + '.' + std::to_string(label_++); }
    ValueId constant(std::int64_t value, Type type) { const auto id = builder_.fresh(); builder_.emit(ConstantInt{id, std::to_string(value), std::move(type)}); return id; }
    ValueId load(std::size_t p) { const auto id = builder_.fresh(); builder_.emit(LoadLocal{id, source_.parameters[p].name, source_.parameters[p].type}); return id; }
    ValueId compare(ValueId value, std::int64_t expected, Type type, std::string op = "==") {
        const auto literal = constant(expected, type); const auto out = builder_.fresh();
        builder_.emit(Binary{out, std::move(op), value, literal, std::move(type), Type::simple(TypeKind::Bool)}); return out;
    }
    ValueId query(const std::string& symbol, std::size_t p, Type result, std::optional<ValueId> axis = {}) {
        Function declaration; declaration.external_symbol = symbol; declaration.result = result;
        declaration.parameters.push_back({"tensor", source_.parameters[p].type, true, false, true});
        if (axis) declaration.parameters.push_back({"axis", Type::simple(TypeKind::Nat64)});
        declaration.name = "$dispatch.query." + platform::sha256_hex(symbol + type_name(source_.parameters[p].type)).substr(0, 24);
        const auto found = std::find_if(queries_.begin(), queries_.end(), [&](const auto& q) { return q.name == declaration.name; });
        if (found == queries_.end()) queries_.push_back(declaration);
        const auto address = builder_.fresh(); builder_.emit(AddressLocal{address, source_.parameters[p].name});
        std::vector<CallArgument> arguments{{0, address}};
        if (axis) arguments.push_back({*axis, {}});
        const auto out = builder_.fresh(); builder_.emit(Call{out, declaration.name, std::move(arguments), result}); return out;
    }
    void branch(ValueId condition, const std::string& failure) {
        const auto next = label("guard"); builder_.emit(Branch{condition, next, failure}); block(next);
    }
    void emit_guard(const Guard& guard, const std::string& failure) {
        const auto i32 = Type::simple(TypeKind::Int32), i64 = Type::simple(TypeKind::Int64), u64 = Type::simple(TypeKind::Nat64);
        switch (guard.kind) {
        case Guard::Kind::Backend: branch(compare(query("qcore_tensor_backend", guard.parameter, i32), guard.expected, i32), failure); break;
        case Guard::Kind::Device: branch(compare(query("qcore_tensor_device", guard.parameter, i64), guard.expected ? 0 : -1, i64, guard.expected ? ">=" : "=="), failure); break;
        case Guard::Kind::Tracked: {
            const auto value = load(guard.parameter), tracked = builder_.fresh(); builder_.emit(TensorIsTracked{tracked, value});
            if (guard.expected) branch(tracked, failure);
            else { const auto out = builder_.fresh(); builder_.emit(Unary{out, "!", tracked, Type::simple(TypeKind::Bool)}); branch(out, failure); }
            break;
        }
        case Guard::Kind::Contiguous: branch(compare(query("qcore_tensor_is_contiguous", guard.parameter, i32), guard.expected, i32), failure); break;
        case Guard::Kind::Rank: branch(compare(query("qcore_tensor_rank", guard.parameter, u64), guard.expected, u64), failure); break;
        case Guard::Kind::Extent: {
            const auto axis = constant(static_cast<std::int64_t>(guard.other), u64);
            branch(compare(query("qcore_tensor_extent", guard.parameter, i64, axis), guard.expected, i64), failure); break;
        }
        case Guard::Kind::SameShape: {
            const auto left = query("qcore_tensor_rank", guard.parameter, u64), right = query("qcore_tensor_rank", guard.other, u64);
            const auto equal = builder_.fresh(); builder_.emit(Binary{equal, "==", left, right, u64, Type::simple(TypeKind::Bool)}); branch(equal, failure);
            const auto local = label("axis.local"), loop = label("axis.loop"), body = label("axis.body"), done = label("axis.done");
            builder_.emit(DeclareLocal{local, u64, "", 0, 0}); const auto zero = constant(0, u64); builder_.emit(StoreLocal{local, zero, u64}); builder_.emit(Jump{loop});
            block(loop); const auto axis = builder_.fresh(); builder_.emit(LoadLocal{axis, local, u64});
            const auto more = builder_.fresh(); builder_.emit(Binary{more, "<", axis, left, u64, Type::simple(TypeKind::Bool)}); builder_.emit(Branch{more, body, done});
            block(body); const auto a = query("qcore_tensor_extent", guard.parameter, i64, axis), b = query("qcore_tensor_extent", guard.other, i64, axis);
            const auto same = builder_.fresh(); builder_.emit(Binary{same, "==", a, b, i64, Type::simple(TypeKind::Bool)}); branch(same, failure);
            const auto one = constant(1, u64), increment = builder_.fresh(); builder_.emit(Binary{increment, "+", axis, one, u64, u64, 0, 0, true}); builder_.emit(StoreLocal{local, increment, u64}); builder_.emit(Jump{loop}); block(done); break;
        }
        }
    }
    void forward(const Function& target) {
        std::vector<CallArgument> arguments;
        for (std::size_t i = 0; i < function_.parameters.size(); ++i) {
            const auto& p = function_.parameters[i]; const auto& t = target.parameters[i];
            if (p.writable || (target.external_symbol && (p.type.kind == TypeKind::Tensor || p.type.kind == TypeKind::String || p.type.kind == TypeKind::Bin))) {
                const auto address = builder_.fresh(); builder_.emit(AddressLocal{address, p.name}); arguments.push_back({0, address});
            } else {
                auto value = builder_.fresh(); builder_.emit(LoadLocal{value, p.name, p.type, !p.borrowed && !t.borrowed && owns_managed(p.type)});
                if (p.borrowed && !t.borrowed && requires_value_clone(p.type)) {
                    const auto clone = builder_.fresh(); builder_.emit(Clone{clone, value, p.type}); value = clone;
                }
                arguments.push_back({value, {}});
            }
        }
        const auto out = function_.result.kind == TypeKind::Void || function_.result.kind == TypeKind::Never ? ValueId{} : builder_.fresh();
        builder_.emit(Call{out, target.name, std::move(arguments), function_.result, {}, function_.result.kind == TypeKind::Never});
        if (function_.result.kind == TypeKind::Void) builder_.emit(ReturnVoid{});
        else if (function_.result.kind != TypeKind::Never) builder_.emit(Return{out, function_.result});
    }
};
} // namespace
RuleDispatchReport RuleDispatchPass::run(ir::Module& module, const RuleRegistry& registry,
                                         ir::analysis::AnalysisCache* analyses) {
    RuleDispatchReport report;
    const auto& bindings = registry.bindings();
    const auto& rules = registry.conditional_rules();
    for (const auto& table : registry.normalized_tables())
        if (table.schema_revision == 2 && table.normalized && table.normalized->kind == CompilerExtensionTableKind::Donation)
            report.unavailable.push_back({table.extension + ":" + table.name,
                "donation bind/alias/transfer guards are unavailable to ordinary rule dispatch"});
    if (std::none_of(rules.begin(), rules.end(), [](const auto& rule) { return rule.runtime_dispatch; })) return report;
    const FunctionIndex functions(module);
    std::vector<Function> generated, queries;
    std::vector<Rewrite> rewrites;
    std::map<std::string, std::string> cache;
    const auto original_count = module.functions.size();
    for (std::size_t f = 0; f < original_count; ++f) {
        const auto& caller = module.functions[f];
        if (caller.dispatch_stub || caller.external_symbol) continue;
        const auto facts = StaticTensorFactsAnalysis::run(caller);
        for (std::size_t b = 0; b < caller.blocks.size(); ++b) {
            std::unordered_map<std::string, std::string> policies;
            for (std::size_t i = 0; i < caller.blocks[b].instructions.size(); ++i) {
                if (b > std::numeric_limits<analysis::BlockId>::max() || i > std::numeric_limits<std::uint32_t>::max())
                    throw std::overflow_error("dispatch instruction index exceeds IR boundary");
                const auto* call = std::get_if<Call>(&caller.blocks[b].instructions[i]);
                if (!call) continue;
                if (const auto setter = bindings.execution_policy_setters.find(call->callee); setter != bindings.execution_policy_setters.end()) {
                    std::map<std::string, std::set<std::string>> values;
                    for (const auto& contract : setter->second) values[contract.extension].insert(contract.value);
                    for (const auto& [extension, ids] : values) {
                        if (ids.size() == 1) policies[extension] = *ids.begin();
                        else policies.erase(extension); // Conflicting opaque IDs are unavailable.
                    }
                    continue;
                }
                const auto active = std::exchange(policies, {});
                const auto* source = functions.find(call->callee);
                if (!source || source->dispatch_stub || source->parameters.size() != call->args.size()) continue;
                const auto first = std::find_if(source->parameters.begin(), source->parameters.end(),
                    [](const auto& p) { return p.type.kind == TypeKind::Tensor; });
                if (first == source->parameters.end()) continue;
                const auto tensor = static_cast<std::size_t>(first - source->parameters.begin());
                if (!call->args[tensor].value && !call->args[tensor].writable_address) continue;
                StaticTensorFacts fact;
                if (const auto found = facts.find(call->args[tensor].value); found != facts.end()) fact = found->second;
                if (!fact.has_type) { fact.has_type = true; fact.type = first->type; }
                std::vector<Candidate> candidates;
                bool unavailable = false;
                for (const auto& rule : rules) {
                    if (!rule.runtime_dispatch || !bindings.call_has_operation(*call, rule.operation)) continue;
                    const analysis::AliasInfo* aliases = nullptr;
                    const analysis::Liveness* liveness = nullptr;
                    if (analyses && (rule.constraints.owned || rule.constraints.last_use)) {
                        aliases = &analyses->aliases(f);
                        if (rule.constraints.last_use) liveness = &analyses->liveness(f);
                    }
                    auto plan = guards_for(rule, *source, *call, tensor, fact, active,
                                           aliases, liveness, {static_cast<analysis::BlockId>(b), static_cast<std::uint32_t>(i)});
                    if (plan.status == GuardPlan::Status::Contradicted) continue;
                    if (plan.status == GuardPlan::Status::Unavailable) {
                        report.unavailable.push_back({rule.reference, plan.reason}); unavailable = true; break;
                    }
                    const auto replacements = bindings.operation_functions.find(rule.replacement);
                    const auto original_traits = bindings.operation_traits.find(rule.operation);
                    const auto replacement_traits = bindings.operation_traits.find(rule.replacement);
                    if (replacements == bindings.operation_functions.end() || original_traits == bindings.operation_traits.end() ||
                        replacement_traits == bindings.operation_traits.end() ||
                        !replacement_keeps_safety_traits(original_traits->second, replacement_traits->second)) continue;
                    const Function* target = nullptr; std::size_t matches = 0;
                    for (const auto& name : replacements->second) {
                        const auto* candidate = functions.find(name);
                        if (candidate && candidate->name != source->name &&
                            compatible(*source, *candidate, call->result, rule.stage)) { target = candidate; ++matches; }
                    }
                    if (matches != 1) {
                        report.unavailable.push_back({rule.reference, matches ? "ambiguous replacement contract" : "replacement contract unavailable"});
                        unavailable = true; break;
                    }
                    const auto contract = registry.declarations().operations.find(rule.replacement);
                    if (contract == registry.declarations().operations.end()) {
                        report.unavailable.push_back({rule.reference, "replacement entry contract unavailable"}); unavailable = true; break;
                    }
                    const auto& fields = contract->second.contract.fields;
                    if (fields.contains("preconditions")) {
                        report.unavailable.push_back({rule.reference, "replacement preconditions require unavailable predicate proof"}); unavailable = true; break;
                    }
                    if (const auto alias = fields.find("inplace"); alias != fields.end() && !std::get<std::map<std::string,std::string>>(alias->second).empty()) {
                        report.unavailable.push_back({rule.reference, "replacement inplace contract requires unavailable donation bind/alias guard"}); unavailable = true; break;
                    }
                    if (const auto tracked = fields.find("untracked_only"); tracked != fields.end() && std::get<bool>(tracked->second))
                        for (std::size_t parameter = 0; parameter < target->parameters.size(); ++parameter)
                            if (target->parameters[parameter].type.kind == TypeKind::Tensor)
                                plan.guards.push_back({Guard::Kind::Tracked, parameter, 0, 0});
                    candidates.push_back({target, &rule, std::move(plan.guards)});
                    if (candidates.back().guards.empty()) break; // Remaining priority alternatives cannot be selected.
                }
                if (unavailable || candidates.empty()) continue;
                if (candidates.size() == 1 && candidates.front().guards.empty() && same_abi(*source, *candidates.front().target)) {
                    rewrites.push_back({f, b, i, candidates.front().target->name}); continue;
                }
                std::string key = source->name + ':' + type_name(call->result);
                for (const auto& p : source->parameters)
                    key += ':' + p.name + ':' + type_name(p.type) + ':' + std::to_string(p.writable) + std::to_string(p.borrowed) + std::to_string(p.is_const);
                for (const auto& candidate : candidates) {
                    key += ':' + candidate.rule->reference + ':' + candidate.target->name;
                    for (const auto& guard : candidate.guards)
                        key += ':' + std::to_string(static_cast<unsigned>(guard.kind)) + '/' + std::to_string(guard.parameter) + '/' + std::to_string(guard.other) + '/' + std::to_string(guard.expected);
                }
                auto found = cache.find(key);
                if (found == cache.end()) {
                    const auto name = "$dispatch." + platform::sha256_hex(key);
                    if (functions.find(name)) { report.unavailable.push_back({candidates.front().rule->reference, "dispatch name already present"}); continue; }
                    generated.push_back(StubBuilder(*source, call->result, name, queries).build(candidates));
                    found = cache.emplace(std::move(key), name).first;
                }
                rewrites.push_back({f, b, i, found->second});
            }
        }
    }
    // No pointers into Module are retained after this boundary. Only exact
    // instruction indexes are used, and append starts after every rewrite.
    for (const auto& rewrite : rewrites) std::get<Call>(module.functions[rewrite.function].blocks[rewrite.block].instructions[rewrite.instruction]).callee = rewrite.callee;
    report.rewritten_calls = rewrites.size(); report.generated_stubs = generated.size();
    for (auto& query : queries) {
        if (std::none_of(module.functions.begin(), module.functions.end(), [&](const auto& existing) { return existing.name == query.name; }))
            module.functions.push_back(std::move(query));
    }
    for (auto& function : generated) module.functions.push_back(std::move(function));
    std::sort(report.unavailable.begin(), report.unavailable.end(), [](const auto& a, const auto& b) { return std::tie(a.reference, a.reason) < std::tie(b.reference, b.reason); });
    report.unavailable.erase(std::unique(report.unavailable.begin(), report.unavailable.end(), [](const auto& a, const auto& b) { return a.reference == b.reference && a.reason == b.reason; }), report.unavailable.end());
    return report;
}
} // namespace quidra::optimizer
