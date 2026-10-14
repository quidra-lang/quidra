#include "ir/analysis/analysis_cache.hpp"
#include "ir/analysis/object_bindings.hpp"
#include "ir/analysis/summary_facts.hpp"
#include "ir/inline_binding_origins.hpp"
#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "quidra/compiler.hpp"
#include "quidra/lowering.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
using namespace quidra::optimizer;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
enum class Case { Fixed, HolderReplacement, FieldRebind };
std::string source(Case kind) {
  std::string text = R"(class Cell(int32 value)
class Pair(Cell first, Cell second)
class Binder<M>(local M &input)
    private M &model
    int32 counter
    &model = &input
    counter = 0

    void .step()
        counter = counter + 1
        for Cell &cell = &reflect.each<Cell>(&model)[]
            counter = counter + cell.value
        return

    void .reset(M &input)
        &model = &input
        return

void drive(Pair &input, Pair &other)
    int32 padding = 0
)";
  // Authored host code provides real caller/module growth capacity. No source
  // rows, semantic flags or return/birth receipts are injected by the fixture.
  for (int i = 0; i < 32; ++i)
    text += "    padding = padding + 1\n";
  text += "    Binder<Pair> held = Binder<Pair>(&input)\n";
  text += "    for int32 iteration < 3\n";
  if (kind == Case::HolderReplacement)
    text += "        held = Binder<Pair>(&other)\n";
  if (kind == Case::FieldRebind)
    text += "        .held.reset(&other)\n";
  text += R"(        .held.step()
    return
Pair first = Pair(Cell(1), Cell(2))
Pair second = Pair(Cell(3), Cell(4))
drive(&first, &second)
void reserve()
    int32 value = 0
)";
  for (int i = 0; i < 100; ++i)
    text += "    value = value + 1\n";
  text += "    return\n";
  return text;
}
std::size_t function(const Module &module, std::string_view name) {
  std::optional<std::size_t> result;
  for (std::size_t i = 0; i < module.functions.size(); ++i)
    if (const auto &declaration = module.functions[i].declaration;
        declaration &&
        declaration->kind == DeclarationIdentity::Kind::Function &&
        declaration->source_name == name &&
        !module.functions[i].shape_wrapper) {
      require(!result, "source declaration is ambiguous");
      result = i;
    }
  require(result.has_value(), "source function declaration is absent");
  return *result;
}
Type leaf_type(const BindingSources &sources) {
  std::optional<Type> result;
  for (const auto &[lookup, layout] : sources.classes)
    if (layout.identity.source_name == "Cell" &&
        layout.identity.generic_arguments.empty()) {
      require(!result, "source Cell identity is ambiguous");
      require(layout.complete, "source Cell layout is incomplete");
      result = Type::class_type(lookup);
    }
  require(result.has_value(), "source Cell layout is absent");
  return *result;
}
ObjectBindingRequest call_request(const Module &module, AnalysisCache &cache,
                                  std::size_t host, const Type &target) {
  const auto &fn = module.functions[host];
  const auto &loops = cache.loops(host);
  for (BlockId b = 0; b < fn.blocks.size(); ++b)
    for (std::uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i)
      if (const auto *call = std::get_if<Call>(&fn.blocks[b].instructions[i]))
        for (const auto &callee : module.functions)
          if (callee.name == call->callee && callee.declaration &&
              callee.declaration->kind == DeclarationIdentity::Kind::Method &&
              callee.declaration->owner_class == "Binder" &&
              callee.declaration->source_name == "step") {
            require(!call->args.empty() && loops.innermost_of_block[b] >= 0,
                    "source step receiver/loop is absent");
            const auto &arg = call->args.front();
            return {host,
                    static_cast<std::uint32_t>(loops.innermost_of_block[b]),
                    {b, i},
                    arg.value ? arg.value : arg.writable_address.value_or(0),
                    target};
          }
  throw std::runtime_error("actual source step Call is absent");
}
ObjectBindingRequest scoped_request(const Module &module, AnalysisCache &cache,
                                    std::size_t host, const Type &target) {
  const auto &fn = module.functions[host];
  const auto &sources = cache.binding_sources();
  const auto rows = sources.scopes.find(fn.name);
  require(rows != sources.scopes.end(),
          "accepted inliner produced no scope owner");
  for (const auto &[id, row] : rows->second) {
    (void)id;
    if (row.source.declaration.kind != DeclarationIdentity::Kind::Method ||
        row.source.declaration.owner_class != "Binder" ||
        row.source.declaration.source_name != "step")
      continue;
    require(valid_scoped_binding_source(module, fn, row) &&
                row.returns_complete,
            "actual step source/return receipt is incomplete");
    require(row.source.availability.traversals &&
                !row.source.traversals.empty() && !row.parameters.empty(),
            "copied step traversal/formal bind is absent");
    const auto at = row.source.traversals.front().at;
    const auto &loops = cache.loops(host);
    require(at.block < fn.blocks.size() &&
                at.index <= fn.blocks[at.block].instructions.size() &&
                loops.innermost_of_block[at.block] >= 0,
            "copied step anchor is not an actual loop boundary");
    const auto &receiver = row.parameters.front();
    return {host,
            static_cast<std::uint32_t>(loops.innermost_of_block[at.block]), at,
            receiver.value ? receiver.value : receiver.address.value_or(0),
            target};
  }
  throw std::runtime_error("actual accepted step scope is absent");
}
ObjectBindingResult inspect(const Module &module, bool copied) {
  AnalysisCache cache(module);
  const auto host = function(module, "drive");
  const auto &sources = cache.binding_sources();
  const auto target = leaf_type(sources);
  const auto request = copied ? scoped_request(module, cache, host, target)
                              : call_request(module, cache, host, target);
  const auto report = object_binding_closure(module, *module.summaries, sources,
                                             cache.binding_proof(), {request});
  require(!report.budget_exhausted && report.results.size() == 1,
          "bounded generic closure did not return its actual request");
  return report.results.front();
}
void check_case(Case kind) {
  auto module = lower(check(source(kind)));
  require(module.summaries && module.binding_origins &&
              module.interface_sources,
          "actual lowering source/effect owners are absent");
  const auto inferred = infer_ir_summary_facts(module, *module.summaries);
  module.summaries = std::make_shared<const ModuleSummaries>(
      apply_ir_summary_facts(module, *module.summaries, inferred));
  const auto before = inspect(module, false);
  require(before.fact.has_value() == (kind == Case::Fixed),
          "source-level unexpanded closure expectation changed");
  const auto registry = RuleRegistry::build(module);
  InliningOptions options;
  const auto inlined = BudgetedInliner::run(module, registry, options);
  require(inlined.changed(),
          "actual transactional inliner made no accepted expansion");
  const auto after = inspect(module, true);
  require(after.fact.has_value() == (kind == Case::Fixed),
          "copied constructor/step changed object-sensitive closure");
  if (after.fact) {
    require(
        after.fact->allocation.kind == ObjectRoot::Allocation &&
            after.fact->inline_birth &&
            after.fact->inline_birth->declaration.kind ==
                DeclarationIdentity::Kind::Constructor &&
            !after.fact->inline_birth->declaration.generic_args.empty(),
        "inlined birth fabricated a Call or lost original generic constructor");
    require(after.fact->root_holder_available && !after.fact->scoped_holder &&
                after.fact->holder == before.fact->holder &&
                after.fact->model == before.fact->model &&
                after.fact->paths == before.fact->paths,
            "copied namespaces or model membership were flattened/changed");
    require(!after.fact->inline_birth->returns.empty() &&
                !after.fact->inline_birth->parameters.empty(),
            "constructor birth dropped the owned return/formal receipts");
  }
}
} // namespace
int main() {
  try {
    check_case(Case::Fixed);
    check_case(Case::HolderReplacement);
    check_case(Case::FieldRebind);
    std::puts("source inline object bindings: ok");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
