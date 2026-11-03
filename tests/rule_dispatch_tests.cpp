#include "optimizer/rule_dispatch.hpp"
#include "quidra/compiler_extension.hpp"
#include "compiler_extension_expression.hpp"
#include "quidra/native_extension.h"
#include <iostream>
#include <map>
#include <stdexcept>

using namespace quidra;
using namespace quidra::optimizer;
using namespace quidra::ir;
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static const Type tensor = Type::tensor(Type::simple(TypeKind::Real32), 2, {2, 3});
static Function function(std::string name, std::int64_t result) {
    Function out; out.name = "alias." + name; out.parameters = {{"value", tensor, false, true}, {"other", tensor, false, true}};
    out.result = Type::simple(TypeKind::Int64);
    out.blocks = {{"entry", {ConstantInt{1, std::to_string(result), out.result}, Return{1, out.result}}}};
    DeclarationIdentity d; d.package = "generic"; d.source_name = name; d.signature = "(tensor<real32>,tensor<real32>)";
    d.parameters = {{"value", "tensor<real32>", false, false}, {"other", "tensor<real32>", false, false}};
    d.result_type = "int64"; out.declaration = std::move(d); return out;
}
static Module fixture(std::string constraint = {}) {
    Module module; module.functions = {function("source", 1), function("high", 2), function("low", 3)};
    for (const auto& f : module.functions) module.declaration_catalog.push_back(*f.declaration);
    std::string descriptor = R"toml([extension]
version = 2
phase = "tensor-program"
[operation.source]
function = "source"
traits = "tensor,pure"
[operation.high]
function = "high"
traits = "tensor,pure"
[operation.low]
function = "low"
traits = "tensor,pure"
[backend.high]
operation = "source"
replacement = "high"
backend = "metal"
tracked = false
layout = "contiguous"
rank = 2
shape = "2,3"
same_shape = "value,other"
priority = 50
)toml" + constraint + R"toml([backend.low]
operation = "source"
replacement = "low"
backend = "cuda"
priority = 10
)toml";
    module.compiler_extensions.push_back(parse_compiler_extension_descriptor("generic", "graph", "/generic/package", "/generic/package/graph.toml", descriptor));
    Function caller; caller.name = "caller"; caller.parameters = {{"value", tensor, false, true}, {"other", tensor, false, true}};
    caller.result = Type::simple(TypeKind::Int64); caller.blocks = {{"entry", {
        LoadLocal{1, "value", tensor}, LoadLocal{2, "other", tensor},
        Call{3, "alias.source", {{1, {}}, {2, {}}}, caller.result}, Return{3, caller.result}}}};
    module.functions.push_back(std::move(caller)); return module;
}
struct DispatchInput { int backend{QCORE_BACKEND_METAL}; bool tracked{}, contiguous{true}; std::vector<std::int64_t> shape{2, 3}, other{2, 3}; bool other_tracked{}; };
// Execute the generated ordinary IR's predicates/control flow against owned
// descriptor facts. Native query names are public ABI identities, not package
// operation names. Unsupported instructions fail the fixture explicitly.
static std::int64_t execute(const Module& module, const Function& stub, const DispatchInput& input) {
    std::map<ValueId, std::int64_t> values; std::map<std::string, std::int64_t> locals{{"value", 0}, {"other", 1}};
    std::string block = "entry";
    for (std::size_t steps = 0; steps < 10000; ++steps) {
        const auto found = std::find_if(stub.blocks.begin(), stub.blocks.end(), [&](const auto& b) { return b.label == block; });
        check(found != stub.blocks.end(), "missing dispatch branch target"); bool jumped = false;
        for (const auto& instruction : found->instructions) {
            if (const auto* n = std::get_if<ConstantInt>(&instruction)) values[n->out] = std::stoll(n->value);
            else if (const auto* n = std::get_if<LoadLocal>(&instruction)) values[n->out] = locals.at(n->name);
            else if (const auto* n = std::get_if<AddressLocal>(&instruction)) values[n->out] = locals.at(n->name);
            else if (const auto* n = std::get_if<StoreLocal>(&instruction)) locals[n->name] = values.at(n->value);
            else if (std::holds_alternative<DeclareLocal>(instruction)) {}
            else if (const auto* n = std::get_if<TensorIsTracked>(&instruction)) values[n->out] = values.at(n->tensor) ? input.other_tracked : input.tracked;
            else if (const auto* n = std::get_if<Unary>(&instruction)) { check(n->op == "!", "unsupported unary guard"); values[n->out] = !values.at(n->operand); }
            else if (const auto* n = std::get_if<Binary>(&instruction)) {
                const auto a = values.at(n->left), b = values.at(n->right);
                if (n->op == "==") values[n->out] = a == b;
                else if (n->op == ">=") values[n->out] = a >= b;
                else if (n->op == "<") values[n->out] = a < b;
                else if (n->op == "+") values[n->out] = a + b;
                else throw std::runtime_error("unsupported binary guard");
            } else if (const auto* n = std::get_if<Call>(&instruction)) {
                const auto target = std::find_if(module.functions.begin(), module.functions.end(), [&](const auto& f) { return f.name == n->callee; });
                check(target != module.functions.end(), "query target not materialized");
                if (!target->external_symbol) return target->name == "alias.high" ? 2 : target->name == "alias.low" ? 3 : 1;
                check(n->args.front().writable_address.has_value() && n->args.front().value == 0 &&
                      target->parameters.front().writable && target->parameters.front().is_const,
                      "query violates typed managed FFI address contract");
                const auto& shape = values.at(*n->args.front().writable_address) ? input.other : input.shape;
                const auto& symbol = *target->external_symbol;
                if (symbol == "qcore_tensor_backend") values[n->out] = input.backend;
                else if (symbol == "qcore_tensor_is_contiguous") values[n->out] = input.contiguous;
                else if (symbol == "qcore_tensor_rank") values[n->out] = static_cast<std::int64_t>(shape.size());
                else if (symbol == "qcore_tensor_extent") { const auto axis = static_cast<std::size_t>(values.at(n->args.at(1).value)); values[n->out] = axis < shape.size() ? shape[axis] : -1; }
                else throw std::runtime_error("unexpected query ABI");
            } else if (const auto* n = std::get_if<Branch>(&instruction)) { block = values.at(n->condition) ? n->if_true : n->if_false; jumped = true; break; }
            else if (const auto* n = std::get_if<Jump>(&instruction)) { block = n->target; jumped = true; break; }
            else throw std::runtime_error("unsupported dispatch instruction");
        }
        check(jumped, "dispatch block lacks terminator");
    }
    throw std::runtime_error("dispatch loop did not terminate");
}
int main() {
    try {
        auto module = fixture(); const auto registry = RuleRegistry::build(module);
        const auto report = RuleDispatchPass::run(module, registry);
        check(report.rewritten_calls == 1 && report.generated_stubs == 1 && report.unavailable.empty(), "unknown predicates did not generate dispatch");
        const auto& call = std::get<Call>(module.functions[3].blocks[0].instructions[2]);
        const auto found = std::find_if(module.functions.begin(), module.functions.end(), [&](const auto& f) { return f.name == call.callee; });
        check(found != module.functions.end() && found->dispatch_stub, "caller not redirected to typed stub");
        check(execute(module, *found, {}) == 2, "highest-priority matching rule not selected");
        DispatchInput input; input.backend = QCORE_BACKEND_CUDA; check(execute(module, *found, input) == 3, "lower matching backend not selected");
        input.backend = QCORE_BACKEND_METAL; input.tracked = true; check(execute(module, *found, input) == 1, "tracked input did not fall back");
        input.tracked = false; input.contiguous = false; check(execute(module, *found, input) == 1, "strided input did not fall back");
        input.contiguous = true; input.shape = {2, 4}; check(execute(module, *found, input) == 1, "declared shape circularly proved its own guard");
        input.shape = {2}; check(execute(module, *found, input) == 1, "declared rank circularly proved its own guard");
        input.shape = {2, 3}; input.other = {2, 4}; check(execute(module, *found, input) == 1, "same_shape did not check actual extents");
        const auto repeated = RuleDispatchPass::run(module, RuleRegistry::build(module)); check(!repeated.changed(), "stub fallback recursively dispatched");
        auto owned = fixture("owned = true\n"); auto unavailable = RuleDispatchPass::run(owned, RuleRegistry::build(owned));
        check(!unavailable.changed() && !unavailable.unavailable.empty(), "missing ownership proof became a donor");
        const auto update = [](Module& m, const std::string& table, const std::string& key, CompilerExtensionValue value, std::string raw) {
            auto model = std::make_shared<CompilerExtensionModel>(*m.compiler_extensions[0].model);
            model->tables.at(table).fields[key] = std::move(value); m.compiler_extensions[0].model = std::move(model);
            m.compiler_extensions[0].tables.at(table)[key] = std::move(raw);
        };
        auto extra = fixture(); update(extra, "operation.low", "untracked_only", true, "true");
        const auto er = RuleDispatchPass::run(extra, RuleRegistry::build(extra)); check(er.changed(), "replacement untracked-only contract did not produce guards");
        const auto& ec = std::get<Call>(extra.functions[3].blocks[0].instructions[2]);
        const auto ei = std::find_if(extra.functions.begin(), extra.functions.end(), [&](const auto& f) { return f.name == ec.callee; });
        DispatchInput tracked_second; tracked_second.backend = QCORE_BACKEND_CUDA; tracked_second.other_tracked = true;
        check(execute(extra, *ei, tracked_second) == 1, "replacement untracked_only silently accepted a tracked second input");
        auto precondition = fixture(); auto predicate = parse_compiler_expression("rank >= 2", CompilerExpression::Category::Predicate, "generic precondition");
        update(precondition, "operation.high", "preconditions", std::move(predicate), "rank >= 2");
        const auto pc = RuleDispatchPass::run(precondition, RuleRegistry::build(precondition));
        check(!pc.changed() && !pc.unavailable.empty(), "unsupported replacement preconditions became unconditional dispatch");
        auto scalar = fixture(); update(scalar, "backend.high", "shape", std::vector<std::int64_t>{}, "");
        auto model = std::make_shared<CompilerExtensionModel>(*scalar.compiler_extensions[0].model); model->tables.at("backend.high").fields.erase("rank"); scalar.compiler_extensions[0].model = model;
        scalar.compiler_extensions[0].tables.at("backend.high").erase("rank");
        const auto sc = RuleDispatchPass::run(scalar, RuleRegistry::build(scalar)); check(sc.changed(), "scalar shape did not produce dispatch");
        const auto& cc = std::get<Call>(scalar.functions[3].blocks[0].instructions[2]);
        const auto ci = std::find_if(scalar.functions.begin(), scalar.functions.end(), [&](const auto& f) { return f.name == cc.callee; });
        check(execute(scalar, *ci, {}) == 1, "shape=[] was treated as an absent constraint");
        DispatchInput rank_zero; rank_zero.shape.clear(); rank_zero.other.clear(); check(execute(scalar, *ci, rank_zero) == 2, "rank-zero inputs did not satisfy explicit scalar shape");
        std::cout << "rule dispatch tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
