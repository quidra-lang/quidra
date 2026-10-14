#include "ir/analysis/object_bindings.hpp"
#include "quidra/ir/initialization_mask.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
static void foreign_constructor_writer() {
  const auto owner = Type::class_type("ir_owner_A"),
             integer = Type::simple(TypeKind::Int64);
  Module module;
  ClassLayout layout;
  layout.name = "ir_owner_A";
  layout.field_names = {"slot"};
  layout.fields = {integer};
  module.classes.push_back(layout);
  DeclarationIdentity declaration;
  declaration.package = "pkg";
  declaration.module_path = "logical";
  declaration.owner_class = "Owner";
  declaration.source_name = "0";
  declaration.kind = DeclarationIdentity::Kind::Constructor;
  declaration.generic_args = {"A"};
  Function fn;
  fn.name = "unrelated_ir_constructor";
  fn.result = owner;
  fn.declaration = declaration;
  fn.parameters = {{"foreign", owner, true}};
  fn.blocks = {{"entry",
                {LoadLocal{1, "foreign", owner}, ConstantInt{2, "1", integer},
                 FieldSet{1, 0, 2, integer}, ClassMake{3, owner, {2}},
                 Return{3, owner}}}};
  module.functions.push_back(fn);
  BindingSources sources;
  sources.classes.emplace("ir_owner_A",
                          BindingClass{{"pkg", "logical", "Owner", {"A"}},
                                       {{"slot", integer, false, true, false}},
                                       true,
                                       true});
  FunctionBindingSource source;
  source.declaration = declaration;
  source.complete = true;
  source.semantic_parameter_for_ir_parameter = {0};
  sources.functions.emplace(fn.name, source);
  ModuleSummaries summaries;
  const auto proof = binding_proof(module, summaries, sources);
  require(proof.fields.size() == 1, "one exact field expected");
  require(
      !proof.fields[0].constructor_or_rebind_only,
      "foreign instance write was incorrectly authorized by constructor role");
}

struct Fixture {
  Type leaf = Type::class_type("ir_leaf"), model = Type::class_type("ir_model"),
       owner = Type::class_type("ir_holder");
  Type integer = Type::simple(TypeKind::Int64),
       void_type = Type::simple(TypeKind::Void),
       error_type = Type::simple(TypeKind::Error);
  Type fallible = Type::union_of({owner, error_type});
  Module module;
  BindingSources sources;
  ModuleSummaries summaries;
  std::map<std::string, semantics::EffectSummary, std::less<>> effects;
  Fixture() {
    ClassLayout l;
    l.name = leaf.class_name;
    module.classes.push_back(l);
    l = {};
    l.name = model.class_name;
    l.field_names = {"first", "second"};
    l.fields = {leaf, leaf};
    module.classes.push_back(l);
    l = {};
    l.name = owner.class_name;
    l.field_names = {"link", "counter"};
    l.fields = {model, integer};
    l.reference_fields = {0};
    module.classes.push_back(l);
    sources.classes.emplace(
        leaf.class_name,
        BindingClass{{"pkg", "unit", "Leaf", {}}, {}, true, true});
    sources.classes.emplace(model.class_name,
                            BindingClass{{"pkg", "unit", "Model", {}},
                                         {{"first", leaf, true, false, false},
                                          {"second", leaf, true, false, false}},
                                         true,
                                         true});
    sources.classes.emplace(
        owner.class_name,
        BindingClass{{"pkg", "unit", "Holder", {"Model"}},
                     {{"link", model, false, true, true},
                      {"counter", integer, false, true, false}},
                     true,
                     true});
    DeclarationIdentity ctor;
    ctor.kind = DeclarationIdentity::Kind::Constructor;
    ctor.package = "pkg";
    ctor.module_path = "unit";
    ctor.owner_class = "Holder";
    ctor.source_name = "0";
    ctor.generic_args = {"Model"};
    Function birth;
    birth.name = "arbitrary_constructor";
    birth.result = fallible;
    birth.declaration = ctor;
    birth.parameters = {{"input", model, true, true}};
    birth.blocks = {
        {"entry",
         {ClassMake{1, owner, {}}, StoreLocal{"built", 1, owner},
          AddressLocal{2, "input"}, BindFieldReference{1, 0, 2, 0},
          LoadLocal{3, "built", owner}, AddressLocal{4, "built"},
          Call{0, "arbitrary_constructor_stage", {{3, 4}}, void_type},
          LoadLocal{5, "built", owner}, ConstantBool{8, true},
          Branch{8, "success", "failure"}}},
        {"success",
         {Clone{15, 5, owner}, VariantMake{6, 0, 15, fallible, owner},
          Return{6, fallible}}},
        {"failure", {}}};
    ErrorMake error{};
    error.out = 9;
    birth.blocks[2].instructions = {error,
                                    VariantMake{10, 1, 9, fallible, error_type},
                                    Return{10, fallible}};
    module.functions.push_back(birth);
    source(birth, ctor, {0});
    sources.functions.at(birth.name)
        .local_storage_paths.emplace(
            "built",
            semantics::StoragePath{semantics::StorageRoot::local("built"), {}});
    auto method = ctor;
    method.kind = DeclarationIdentity::Kind::Method;
    method.source_name = "change";
    method.mutation_marker = true;
    Function stage;
    stage.name = "arbitrary_constructor_stage";
    stage.result = void_type;
    stage.declaration = method;
    stage.parameters = {{"receiver", owner, true, true}};
    stage.blocks = {
        {"entry",
         {LoadLocal{1, "receiver", owner}, ConstantInt{2, "1", integer},
          FieldSet{1, 1, 2, integer}, ReturnVoid{}}}};
    module.functions.push_back(stage);
    source(stage, method, {0});
    sources.functions.at(stage.name).receiver_ir_parameter = 0;
    sources.functions.at(stage.name).availability.receiver = true;
    Function step = stage;
    step.name = "arbitrary_mutating_step";
    module.functions.push_back(step);
    source(step, method, {0});
    sources.functions.at(step.name).receiver_ir_parameter = 0;
    sources.functions.at(step.name).availability.receiver = true;
    add_summary(birth.name, {}, false, true);
    add_summary(stage.name, {{semantics::PathStep::field_named("counter")}},
                true, false);
    add_summary(step.name, {{semantics::PathStep::field_named("counter")}},
                true, false);
    DeclarationIdentity user;
    user.package = "pkg";
    user.module_path = "unit";
    user.source_name = "user";
    Function caller;
    caller.name = "arbitrary_user";
    caller.result = void_type;
    caller.declaration = user;
    caller.parameters = {{"input", model, true, true},
                         {"other", model, true, true}};
    const auto max = std::numeric_limits<ValueId>::max();
    caller.blocks = {
        {"entry",
         {LoadLocal{max - 1, "input", model}, AddressLocal{max, "input"},
          Call{7, birth.name, {{max - 1, max}}, fallible},
          VariantPayload{8, 7, owner}, StoreLocal{"held", 8, owner},
          Jump{"head"}}},
        {"head", {ConstantBool{50, true}, Branch{50, "body", "exit"}}},
        {"body",
         {LoadLocal{10, "held", owner}, AddressLocal{11, "held"},
          Call{0, step.name, {{10, 11}}, void_type}, Jump{"head"}}},
        {"exit", {ReturnVoid{}}}};
    module.functions.push_back(caller);
    source(caller, user, {0, 1});
    sources.functions.at(caller.name)
        .local_storage_paths.emplace(
            "held",
            semantics::StoragePath{semantics::StorageRoot::local("held"), {}});
    Function walk;
    walk.name = "unrelated_typed_traversal";
    walk.result = void_type;
    auto walk_origin = user;
    walk_origin.source_name = "walk";
    walk.declaration = walk_origin;
    walk.parameters = {{"root", model, false, true}};
    walk.blocks = {{"entry", {LoadLocal{1, "root", model}, ReturnVoid{}}}};
    module.functions.push_back(walk);
    source(walk, walk_origin, {0});
    auto &traversal_source = sources.functions.at(walk.name);
    traversal_source.availability.traversals = true;
    BindingTraversalOrigin traversal;
    traversal.kind = BindingTraversalOrigin::Kind::Each;
    traversal.at = {0, 1};
    traversal.root = 1;
    traversal.root_type = model;
    traversal.target = leaf;
    traversal.paths = {{semantics::PathStep::field_named("first")},
                       {semantics::PathStep::field_named("second")}};
    traversal.complete = true;
    traversal.writable = true;
    traversal_source.traversals.push_back(traversal);
  }
  void source(const Function &fn, const DeclarationIdentity &declaration,
              std::vector<std::optional<std::size_t>> mapping) {
    FunctionBindingSource origin;
    origin.declaration = declaration;
    origin.complete = true;
    origin.semantic_parameter_for_ir_parameter = std::move(mapping);
    origin.availability.parameter_map = true;
    origin.availability.local_storage_paths = true;
    origin.availability.rebind_events = true;
    sources.functions.emplace(fn.name, std::move(origin));
  }
  void add_summary(const std::string &name,
                   std::vector<std::vector<semantics::PathStep>> writes,
                   bool receiver, bool escape) {
    semantics::EffectSummary effect;
    effect.exposed_writes.resize(1);
    effect.exposed_reads.resize(1);
    effect.mutates_receiver = receiver;
    effect.mutates_shared_of_parameter = {false};
    effect.reference_target_escapes = {escape};
    for (const auto &path : writes)
      effect.exposed_writes[0].insert(
          {semantics::StorageRoot::parameter(0), path});
    auto [it, added] = effects.emplace(name, std::move(effect));
    if (!added)
      it->second = std::move(effect);
    FunctionSummary summary;
    summary.semantic = &it->second;
    summary.effects_complete = true;
    summary.effects = 0;
    summary.semantic_parameter_for_ir_parameter = {0};
    summaries.by_function.insert_or_assign(name, std::move(summary));
  }
  ObjectBindingReport run(ObjectBindingOptions options = {}) {
    const auto proof = binding_proof(module, summaries, sources);
    return object_binding_closure(module, summaries, sources, proof,
                                  {{3, 0, {2, 2}, 10, leaf}}, options);
  }
};
static void closure_cases() {
  Fixture valid;
  auto report = valid.run();
  require(report.results.size() == 1 && report.results[0].fact.has_value(),
          "disjoint mutable state and fixed-point loop identity did not close");
  const auto &fact = *report.results[0].fact;
  require(
      fact.allocation.at == InstrRef{0, 2} &&
          fact.model.root.kind == ObjectRoot::Parameter &&
          fact.model.root.parameter == 0 && fact.paths.size() == 2,
      "allocation and bound model paths are not exact independent identities");
  require(report.constructors.at("arbitrary_constructor").available,
          "payload mutation destroyed fresh successful constructor identity");
  Fixture escaped_result;
  Function capture;
  capture.name = "known_reference_capture";
  capture.result = escaped_result.void_type;
  capture.parameters = {{"result", escaped_result.owner, true, true}};
  capture.blocks = {{"entry", {ReturnVoid{}}}};
  escaped_result.module.functions.push_back(capture);
  escaped_result.add_summary(capture.name, {}, false, true);
  escaped_result.module.functions[0].blocks[1].instructions = {
      Clone{15, 5, escaped_result.owner},
      StoreLocal{"escaping_result", 15, escaped_result.owner},
      LoadLocal{16, "escaping_result", escaped_result.owner},
      AddressLocal{17, "escaping_result"},
      Call{0, capture.name, {{16, 17}}, escaped_result.void_type},
      LoadLocal{18, "escaping_result", escaped_result.owner, true},
      VariantMake{6, 0, 18, escaped_result.fallible, escaped_result.owner},
      Return{6, escaped_result.fallible}};
  const auto escaped_report = escaped_result.run();
  require(
      !escaped_report.constructors.contains("arbitrary_constructor") ||
          !escaped_report.constructors.at("arbitrary_constructor").available,
      "constructor returned allocation escaped early but remained private");
  require(!escaped_report.results[0].fact,
          "early constructor-result escape retained object binding closure");
  const auto old = binding_proof(valid.module, valid.summaries, valid.sources);
  require(
      !old.constructor_allocations.contains("arbitrary_constructor") ||
          !old.constructor_allocations.at("arbitrary_constructor"),
      "test must exercise the prior payload-knownness constructor obstruction");
  require(report.identities[3].scopes.size() == 4 &&
              report.identities[3].at({2, 2}, 10).available,
          "sparse MAX ValueId loop identity lost at must-join");
  Fixture masked;
  masked.module.classes[2].field_names.push_back(
      std::string(initialization_mask_field));
  masked.module.classes[2].fields.push_back(Type::simple(TypeKind::Nat64));
  auto &constructor_code = masked.module.functions[0].blocks[0].instructions;
  constructor_code.insert(constructor_code.begin() + 4,
                          ConstantInt{60, "1", Type::simple(TypeKind::Nat64)});
  constructor_code.insert(constructor_code.begin() + 5,
                          FieldSet{1, 2, 60, Type::simple(TypeKind::Nat64)});
  require(masked.run().results[0].fact.has_value(),
          "typed hidden initialization mask write killed binding identity");
  Fixture rebound;
  auto &code = rebound.module.functions[3].blocks[0].instructions;
  code.insert(code.end() - 1, LoadLocal{12, "held", rebound.owner});
  code.insert(code.end() - 1, AddressLocal{13, "other"});
  code.insert(code.end() - 1, BindFieldReference{12, 0, 13, 0});
  require(!rebound.run().results[0].fact,
          "reference binding changed between birth and loop but certificate "
          "survived");
  Fixture inner;
  inner.effects.at("arbitrary_mutating_step").exposed_writes[0].clear();
  inner.effects.at("arbitrary_mutating_step")
      .exposed_writes[0]
      .insert({semantics::StorageRoot::parameter(0),
               {semantics::PathStep::field_named("link")}});
  require(!inner.run().results[0].fact,
          "loop/backedge binding write was mistaken for payload-only mutation");
  Fixture opaque;
  opaque.module.functions[3].blocks[2].instructions.insert(
      opaque.module.functions[3].blocks[2].instructions.end() - 1,
      Call{0, "opaque", {}, opaque.void_type});
  require(!opaque.run().results[0].fact,
          "opaque ambient call left an object binding certificate");
  Fixture parallel;
  ParallelBlock node{};
  parallel.module.functions[3].blocks[2].instructions.insert(
      parallel.module.functions[3].blocks[2].instructions.end() - 1, node);
  require(!parallel.run().results[0].fact,
          "parallel capture/unknown closure was ignored");
  Fixture container;
  container.module.functions[3].blocks[0].instructions.insert(
      container.module.functions[3].blocks[0].instructions.end() - 1,
      ArrayMake{80, {8}, Type::array(container.owner)});
  require(!container.run().results[0].fact,
          "object stored in a container retained binding closure");
  Fixture dynamic;
  dynamic.sources.functions.at("unrelated_typed_traversal")
      .traversals[0]
      .complete = false;
  require(!dynamic.run().results[0].fact,
          "incomplete reflected traversal became a fixed target list");
  Fixture holder;
  auto &holding = holder.module.functions[3].blocks[2].instructions;
  holding.insert(holding.end() - 1, ClassMake{90, holder.owner, {}});
  holding.insert(holding.end() - 1, StoreLocal{"held", 90, holder.owner});
  require(
      !holder.run().results[0].fact,
      "whole holder replacement on a backedge retained the original object");
  require(!valid.run({0}).results[0].fact,
          "zero work budget fabricated a certificate");
}
static void add_scalar_root_abi(Fixture &fixture) {
  const auto scalar = Type::simple(TypeKind::Int64);
  for (const auto index : {0U, 1U, 2U}) {
    auto &function = fixture.module.functions[index];
    function.parameters.push_back({"scalar_metadata", scalar});
    fixture.sources.functions.at(function.name)
        .semantic_parameter_for_ir_parameter.push_back(std::nullopt);
    fixture.summaries.by_function.at(function.name)
        .semantic_parameter_for_ir_parameter.push_back(std::nullopt);
  }
  auto &entry = fixture.module.functions[0].blocks[0].instructions;
  entry.insert(entry.begin() + 3, LoadLocal{1001, "scalar_metadata", scalar});
  for (auto &instruction : entry) {
    if (auto *binding = std::get_if<BindFieldReference>(&instruction))
      binding->root = 1001;
    if (auto *call = std::get_if<Call>(&instruction))
      call->args.push_back({1001, {}});
  }
  auto &caller = fixture.module.functions[3];
  caller.blocks[0].instructions.insert(caller.blocks[0].instructions.begin(),
                                       ConstantInt{1000, "0", scalar});
  for (auto &block : caller.blocks)
    for (auto &instruction : block.instructions)
      if (auto *call = std::get_if<Call>(&instruction))
        call->args.push_back({1000, {}});
}
static void scalar_root_abi_cases() {
  Fixture scalar;
  add_scalar_root_abi(scalar);
  const auto scalar_report = scalar.run();
  require(scalar_report.results[0].fact.has_value(),
          "actual by-value scalar binding-root ABI invalidated object closure");
  require(
      !scalar_report.results[0].fact->calls.empty() &&
          std::all_of(scalar_report.results[0].fact->calls.begin(),
                      scalar_report.results[0].fact->calls.end(),
                      [](const auto &call) { return call.effects_available; }),
      "scalar-root ABI closure published unavailable call evidence");
  Fixture serial;
  add_scalar_root_abi(serial);
  serial.module.functions[3].blocks[0].instructions[0] = RootSerial{1000};
  require(serial.run().results[0].fact.has_value(),
          "nat64 serial producer and int64 hidden formal ABI did not match");
  Fixture structural;
  add_scalar_root_abi(structural);
  structural.module.functions[2].parameters[1].type = structural.owner;
  std::get<Call>(structural.module.functions[3].blocks[2].instructions[2])
      .args[1] = {10, {}};
  require(!structural.run().results[0].fact,
          "null-mapped structural identity was ignored like a scalar");
  Fixture address;
  add_scalar_root_abi(address);
  address.module.functions[2].parameters[1].type =
      Type::simple(TypeKind::Address);
  auto &body = address.module.functions[3].blocks[2].instructions;
  body.insert(body.begin() + 2, AddressLocal{1002, "held"});
  std::get<Call>(body[3]).args[1] = {1002, {}};
  require(!address.run().results[0].fact,
          "null-mapped address argument retained object closure");
  Fixture writable;
  add_scalar_root_abi(writable);
  writable.module.functions[2].parameters[1].writable = true;
  auto &start = writable.module.functions[3].blocks[0].instructions;
  start.insert(start.begin() + 1,
               StoreLocal{"scalar_slot", 1000, Type::simple(TypeKind::Int64)});
  auto &written = writable.module.functions[3].blocks[2].instructions;
  written.insert(written.begin() + 2, AddressLocal{1002, "scalar_slot"});
  std::get<Call>(written[3]).args[1] = {1000, 1002};
  require(!writable.run().results[0].fact,
          "null-mapped writable scalar argument retained object closure");
  Fixture opaque;
  add_scalar_root_abi(opaque);
  opaque.summaries.by_function.at("arbitrary_mutating_step").effects |=
      effect_bit::Opaque;
  require(!opaque.run().results[0].fact,
          "scalar metadata excused an opaque callee");
  Fixture mismatch;
  add_scalar_root_abi(mismatch);
  mismatch.module.functions[2].parameters[1].type =
      Type::simple(TypeKind::Int32);
  require(!mismatch.run().results[0].fact,
          "null-mapped scalar with a different ABI width was ignored");
}
static void exact_instances() {
  Fixture fixture;
  auto &caller = fixture.module.functions[3];
  auto &entry = caller.blocks[0].instructions;
  entry.insert(entry.end() - 1, LoadLocal{100, "other", fixture.model});
  entry.insert(entry.end() - 1, AddressLocal{101, "other"});
  entry.insert(
      entry.end() - 1,
      Call{102, "arbitrary_constructor", {{100, 101}}, fixture.fallible});
  entry.insert(entry.end() - 1, VariantPayload{103, 102, fixture.owner});
  entry.insert(entry.end() - 1, StoreLocal{"second", 103, fixture.owner});
  fixture.sources.functions.at(caller.name)
      .local_storage_paths.emplace(
          "second",
          semantics::StoragePath{semantics::StorageRoot::local("second"), {}});
  auto &body = caller.blocks[2].instructions;
  body.insert(body.end() - 1, LoadLocal{20, "second", fixture.owner});
  body.insert(body.end() - 1, AddressLocal{21, "second"});
  body.insert(
      body.end() - 1,
      Call{0, "arbitrary_mutating_step", {{20, 21}}, fixture.void_type});
  const auto proof =
      binding_proof(fixture.module, fixture.summaries, fixture.sources);
  const auto report = object_binding_closure(
      fixture.module, fixture.summaries, fixture.sources, proof,
      {{3, 0, {2, 2}, 10, fixture.leaf}, {3, 0, {2, 5}, 20, fixture.leaf}});
  require(report.results[0].fact && report.results[1].fact,
          "independent same-class objects failed binding closure");
  require(
      report.results[0].fact->allocation.at !=
              report.results[1].fact->allocation.at &&
          report.results[0].fact->model.root.parameter == 0 &&
          report.results[1].fact->model.root.parameter == 1,
      "same optimizer class merged allocation sites or model storage targets");
  // An explicitly marked method of a different generic owner cannot write A.
  Module module;
  ClassLayout layout;
  layout.name = "instance_A";
  layout.field_names = {"slot"};
  layout.fields = {fixture.integer};
  module.classes.push_back(layout);
  auto declaration =
      fixture.sources.functions.at("arbitrary_mutating_step").declaration;
  declaration.generic_args = {"B"};
  Function fn;
  fn.name = "foreign_generic_writer";
  fn.declaration = declaration;
  fn.parameters = {{"receiver", Type::class_type("instance_A"), true, true}};
  fn.blocks = {{"entry",
                {LoadLocal{1, "receiver", fn.parameters[0].type},
                 ConstantInt{2, "1", fixture.integer},
                 FieldSet{1, 0, 2, fixture.integer}, ReturnVoid{}}}};
  module.functions.push_back(fn);
  BindingSources sources;
  sources.classes.emplace(
      layout.name, BindingClass{{"pkg", "unit", "Holder", {"A"}},
                                {{"slot", fixture.integer, false, true, false}},
                                true,
                                true});
  FunctionBindingSource source;
  source.declaration = declaration;
  source.complete = true;
  source.rebind = true;
  source.receiver_ir_parameter = 0;
  source.semantic_parameter_for_ir_parameter = {0};
  sources.functions.emplace(fn.name, source);
  ModuleSummaries summaries;
  require(!binding_proof(module, summaries, sources)
               .fields[0]
               .constructor_or_rebind_only,
          "different generic owner falsely authorized an exact rebind writer");
}

static void initialization_cases() {
  const auto aggregate = Type::class_type("masked_initialization_target"),
             integer = Type::simple(TypeKind::Int64),
             list = Type::array(aggregate);
  Module module;
  ClassLayout layout;
  layout.name = aggregate.class_name;
  layout.field_names = {"member", std::string(initialization_mask_field)};
  layout.fields = {integer, Type::simple(TypeKind::Nat64)};
  module.classes.push_back(layout);
  BindingSources sources;
  sources.classes.emplace(
      layout.name, BindingClass{{"pkg", "init", "Aggregate", {}},
                                {{"member", integer, false, true, false}},
                                true,
                                true});
  Function fn;
  fn.name = "entry_initialization";
  fn.parameters = {{"input", aggregate, true, true}};
  fn.blocks = {{"entry",
                {LoadLocal{1, "input", aggregate}, ArrayMake{2, {1}, list},
                 ReturnVoid{}}}};
  DeclarationIdentity declaration;
  declaration.package = "pkg";
  declaration.module_path = "init";
  declaration.source_name = "entry";
  fn.declaration = declaration;
  module.functions.push_back(fn);
  FunctionBindingSource source;
  source.declaration = declaration;
  source.complete = true;
  source.semantic_parameter_for_ir_parameter = {0};
  source.availability.parameter_map = true;
  source.collections.push_back(
      CollectionOrigin{{0, 2}, 2, 1, aggregate, {{}}, true});
  sources.functions.emplace(fn.name, source);
  ModuleSummaries summaries;
  auto proof = binding_proof(module, summaries, sources);
  require(proof.collections[0].paths_available &&
              !proof.collections[0].initialized &&
              !proof.collections[0].available,
          "writable whole-root collect fabricated aggregate initialization");
  module.functions[0].parameters[0].writable = false;
  proof = binding_proof(module, summaries, sources);
  require(proof.collections[0].initialized,
          "actual readonly value entry lost its initialization contract");
  module.functions[0].parameters.clear();
  module.functions[0].blocks[0].instructions = {
      ClassMake{1, aggregate, {}}, ArrayMake{2, {1}, list}, ReturnVoid{}};
  sources.functions.at(fn.name).semantic_parameter_for_ir_parameter.clear();
  proof = binding_proof(module, summaries, sources);
  require(proof.collections[0].paths_available &&
              !proof.collections[0].initialized,
          "missing nonreference ClassMake field fabricated initialization");
  module.functions[0].blocks[0].instructions = {
      ClassMake{1, aggregate, {}}, ConstantInt{3, "7", integer},
      FieldSet{1, 0, 3, integer}, ArrayMake{2, {1}, list}, ReturnVoid{}};
  sources.functions.at(fn.name).collections[0].at.index = 4;
  proof = binding_proof(module, summaries, sources);
  require(proof.collections[0].initialized && proof.collections[0].available,
          "successful actual field write failed to establish aggregate "
          "initialization");
}
int main() {
  try {
    foreign_constructor_writer();
    initialization_cases();
    closure_cases();
    exact_instances();
    scalar_root_abi_cases();
    std::puts("object binding closure: essential checks passed");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
