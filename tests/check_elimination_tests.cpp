#if __has_include("optimizer/check_elimination.hpp")
#include "ir/fact_sources.hpp"
#include "optimizer/check_elimination.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/optimizer.hpp"
#include "optimizer/inspection.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::optimizer;
static void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
static Module fixture() {
  const auto i8 = Type::simple(TypeKind::Int8),
             i64 = Type::simple(TypeKind::Int64),
             integer = Type::simple(TypeKind::Int);
  const auto tensor = Type::tensor(Type::simple(TypeKind::Real32), 2, {2, 7});
  const auto array = Type::array(i8);
  const auto max = std::numeric_limits<ValueId>::max();
  Function fn;
  fn.name = "generic_checks";
  fn.parameters = {{"input", tensor, false, true},
                   {"items", array, false, true}};
  ShapedConstraintCheck shape;
  shape.value = max;
  shape.extents = {1, 2};
  shape.position = {8, 3};
  shape.subject = "input";
  shape.shape_variables = {"rows", "columns"};
  fn.blocks = {
      {"entry",
       {SourceLocation{{8, 3}, "generic.qui", "revision", "owned_node", "stmt"},
        LoadLocal{max, "input", tensor}, ConstantInt{1, "2", i64},
        ConstantInt{2, "7", i64}, shape, Jump{"checked"}}},
      {"checked",
       {shape,
        TensorShape{3, max, Type::array(i64)},
        ArrayLength{4, 3},
        ConstantInt{5, "2", i64},
        ExtentEqualCheck{4, 5, {9, 4}},
        ConstantBool{6, true},
        InitializedCheck{6, InitializedSubject::binding, "ready", {10, 1}},
        ConstantBool{7, false},
        InitializedCheck{7, InitializedSubject::binding, "not_ready", {11, 1}},
        ConstantInt{8, "0", integer},
        ForStepCheck{8, false, {12, 1}},
        ConstantInt{9, "-1", integer},
        ForStepCheck{9, true, {13, 1}},
        ConstantInt{10, "127", i8},
        ConstantInt{11, "1", i8},
        Binary{12, "+", 10, 11, i8, i8, 14, 1},
        ConstantInt{13, "4", i8},
        Binary{14, "+", 13, 11, i8, i8, 15, 1},
        LoadLocal{20, "items", array},
        ArrayGet{21, 20, 8, i8, {16, 1}, false, false, 6, 6},
        ConstantInt{30, std::to_string(abi::bare_integer_layout::inline_max),
                    integer},
        ConstantInt{31, "1", integer},
        Binary{32, "+", 30, 31, integer, integer, 17, 1},
        ConstantInt{33, "2", integer},
        Binary{34, "+", 33, 31, integer, integer, 18, 1},
        ReturnVoid{}}}};
  Module module;
  module.functions.push_back(fn);
  module.fact_sources = std::make_shared<const facts::FactSources>();
  return module;
}
static bool check_at(const Function &fn, std::uint32_t line) {
  for (const auto &block : fn.blocks)
    for (const auto &ins : block.instructions) {
      if (const auto *n = std::get_if<ShapedConstraintCheck>(&ins);
          n && n->position.line == line)
        return true;
      if (const auto *n = std::get_if<InitializedCheck>(&ins);
          n && n->position.line == line)
        return true;
      if (const auto *n = std::get_if<ForStepCheck>(&ins);
          n && n->position.line == line)
        return true;
    }
  return false;
}
// Geometry proofs remove only the accepted compatibility bundle. Declared
// parameter shapes and equal unknown facts do not prove two different roots.
static Module geometry_fixture() {
  Module module;
  Function fn;
  fn.name = "generic_tensor_geometry";
  const auto tensor = Type::tensor(Type::simple(TypeKind::Real32));
  const auto mask = Type::tensor(Type::simple(TypeKind::Bool));
  const auto declared = Type::tensor(Type::simple(TypeKind::Real32), 2, {2, 5});
  const auto natural = Type::simple(TypeKind::Nat64);
  fn.parameters = {{"left", declared, false, true},
                   {"right", declared, false, true},
                   {"equal", tensor, false, true},
                   {"dynamic", tensor, false, true}};
  ShapedConstraintCheck left, right, equal, rank_one;
  left.value = 1;
  left.extents = {10, 11};
  left.position = {71, 1};
  right.value = 2;
  right.extents = {12, 11};
  right.position = {72, 1};
  equal.value = 3;
  equal.extents = {10, 11};
  equal.position = {73, 1};
  rank_one.value = 4;
  rank_one.extents = {10};
  rank_one.position = {74, 1};
  fn.blocks = {
      {"entry",
       {LoadLocal{1, "left", declared}, LoadLocal{2, "right", declared},
        LoadLocal{3, "equal", tensor}, LoadLocal{4, "dynamic", tensor},
        ConstantInt{10, "2", natural}, ConstantInt{11, "5", natural},
        ConstantInt{12, "1", natural},
        TensorBinary{20, "+", 1, 2, tensor, tensor, tensor, {70, 4}}, left,
        right, equal,
        TensorBinary{21, "+", 1, 2, tensor, tensor, tensor, {75, 4}},
        TensorCompare{22, "==", 1, 2, tensor, tensor, mask, {76, 4}},
        TensorCompare{23, "==", 1, 3, tensor, tensor, mask, {77, 4}},
        TensorBinary{24, "*", 4, 4, tensor, tensor, tensor, {78, 4}},
        TensorCompare{25, "==", 4, 4, tensor, tensor, mask, {79, 4}}, rank_one,
        TensorBinary{26, "+", 1, 4, tensor, tensor, tensor, {80, 4}},
        ReturnVoid{}}}};
  module.functions = {fn};
  return module;
}
static void geometry_cases() {
  auto module = geometry_fixture();
  const auto plan = CheckElimination::analyze(module, 0);
  require(plan.ready(), "geometry consumer produced no ready plan");
  bool axis = false, exact = false, same_binary = false, same_compare = false;
  for (const auto &proof : plan.report().certificates) {
    if (const auto *n = std::get_if<TensorBinary>(&proof.original)) {
      require(proof.action == CheckAction::BroadcastFlag &&
                  proof.kind == CheckKind::TensorBroadcast,
              "binary compatibility used the wrong typed action");
      axis |= n->out == 21 && proof.rule == ProofRule::BeforeShape &&
              proof.premises.size() == 2;
      same_binary |= n->out == 24 && proof.rule == ProofRule::SameSsaValue;
      require(n->out != 20 && n->out != 26,
              "unvalidated signature or differing rank was certified");
    }
    if (const auto *n = std::get_if<TensorCompare>(&proof.original)) {
      require(proof.action == CheckAction::ShapeFlag &&
                  proof.kind == CheckKind::TensorShape,
              "comparison compatibility used the wrong typed action");
      exact |= n->out == 23 && proof.rule == ProofRule::BeforeShape;
      same_compare |= n->out == 25 && proof.rule == ProofRule::SameSsaValue;
      require(n->out != 22, "broadcast-only axes proved comparison equality");
    }
  }
  require(axis && exact && same_binary && same_compare,
          "before-site axes or same-SSA compatibility was not materialized");
  require(CheckElimination::apply(module, plan).applied,
          "matching geometry certificate was refused");
  bool first = false, mismatch = false, broadcast = false, compare = false;
  for (const auto &ins : module.functions[0].blocks[0].instructions) {
    if (const auto *n = std::get_if<TensorBinary>(&ins)) {
      if (n->out == 20)
        first = !n->broadcast_proven;
      if (n->out == 26)
        mismatch = !n->broadcast_proven;
      if (n->out == 21) {
        broadcast = n->broadcast_proven;
        require(n->op == "+" && n->left == 1 && n->right == 2 &&
                    n->left_type ==
                        Type::tensor(Type::simple(TypeKind::Real32)) &&
                    n->right_type == n->left_type &&
                    n->result_type == n->left_type && n->position.line == 75 &&
                    n->position.column == 4,
                "geometry flag changed the operation/type/error context");
      }
    }
    if (const auto *n = std::get_if<TensorCompare>(&ins); n && n->out == 23)
      compare = n->shape_proven && n->position.line == 77;
  }
  require(
      first && mismatch && broadcast && compare &&
          check_at(module.functions[0], 71),
      "geometry flags erased mandatory validation or changed unknown paths");
  auto stale = geometry_fixture();
  const auto original = CheckElimination::analyze(stale, 0);
  std::get<TensorBinary>(stale.functions[0].blocks[0].instructions[11])
      .broadcast_proven = true;
  require(!CheckElimination::apply(stale, original).applied,
          "raw geometry flag preimage mutation was ignored");
  auto opaque = geometry_fixture();
  opaque.functions[0].blocks[0].instructions.insert(
      opaque.functions[0].blocks[0].instructions.begin() + 11,
      Call{0,
           "unavailable_descriptor_mutation",
           {},
           Type::simple(TypeKind::Void)});
  const auto killed = CheckElimination::analyze(opaque, 0);
  for (const auto &proof : killed.report().certificates)
    if (const auto *n = std::get_if<TensorBinary>(&proof.original))
      require(n->out != 21,
              "stale axes survived an unknown descriptor mutation");
    else if (const auto *n = std::get_if<TensorCompare>(&proof.original))
      require(n->out != 23,
              "stale comparison axes survived descriptor mutation");
}
int main() {
  try {
    geometry_cases();
    auto module = fixture();
    const auto plan = CheckElimination::analyze(module, 0);
    require(plan.ready(), "valid typed check plan was unavailable");
    require(plan.report().removed >= 4,
            "canonical/prior shape and literal checks were not certified");
    require(CheckElimination::apply(module, plan).applied,
            "matching original preimage was refused");
    require(check_at(module.functions[0], 8),
            "check proved itself from its own refinement");
    require(check_at(module.functions[0], 11) &&
                check_at(module.functions[0], 12),
            "literal failing checks disappeared");
    require(!check_at(module.functions[0], 10) &&
                !check_at(module.functions[0], 13),
            "successful literal checks stayed");
    bool overflow_kept = false, overflow_set = false, inline_kept = false,
         inline_set = false, guard_set = false;
    for (const auto &block : module.functions[0].blocks)
      for (const auto &ins : block.instructions) {
        if (const auto *n = std::get_if<Binary>(&ins)) {
          if (n->out == 12)
            overflow_kept = !n->overflow_proven;
          if (n->out == 14)
            overflow_set = n->overflow_proven;
          if (n->out == 32)
            inline_kept = !n->inline_proven;
          if (n->out == 34)
            inline_set = n->inline_proven;
        }
        if (const auto *n = std::get_if<ArrayGet>(&ins))
          guard_set = n->bounds_proven && n->initialization_proven;
      }
    require(
        overflow_kept && overflow_set && inline_kept && inline_set && guard_set,
        "evaluated flags crossed an ABI/type boundary or missed true guards");
    require(!CheckElimination::apply(module, plan).applied,
            "repeated stale plan was applied");
    Module canonical;
    Function symbolic;
    symbolic.name = "symbolic_extent_identity";
    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32));
    const auto natural = Type::simple(TypeKind::Nat64);
    symbolic.parameters = {{"input", tensor, false, true}};
    ShapedConstraintCheck rank;
    rank.value = 1;
    rank.extents = {std::nullopt};
    symbolic.blocks = {
        {"entry",
         {LoadLocal{1, "input", tensor}, rank,
          TensorShape{2, 1, Type::array(natural)}, ConstantInt{3, "0", natural},
          ArrayGet{4, 2, 3, natural, {}, true, true},
          ConstantInt{5, "1", natural}, Binary{6, "*", 4, 5, natural, natural},
          ExtentEqualCheck{6, 4}, ReturnVoid{}}}};
    canonical.functions.push_back(symbolic);
    const auto algebra = CheckElimination::analyze(canonical, 0);
    bool canonical_identity = false;
    for (const auto &certificate : algebra.report().certificates)
      if (certificate.kind == CheckKind::ExtentEquality &&
          certificate.rule == ProofRule::CanonicalExtentIdentity &&
          certificate.premises.size() == 2 && certificate.premises[0].extent &&
          certificate.premises[0].extent->kind != ShapeExtent::Kind::Known)
        canonical_identity = true;
    require(canonical_identity,
            "canonical symbolic extent identity reduced to constants only");
    Module deep;
    Function chain;
    chain.name = "bounded_sparse_boolean_chain";
    chain.blocks = {{"entry",
                     {ConstantBool{1, true},
                      InitializedCheck{1, InitializedSubject::binding,
                                       "earlier_success"}}}};
    auto previous = ValueId{1};
    for (std::uint32_t i = 0; i < 600; ++i) {
      const auto next = std::numeric_limits<ValueId>::max() - i;
      chain.blocks[0].instructions.push_back(
          Clone{next, previous, Type::simple(TypeKind::Bool)});
      previous = next;
    }
    chain.blocks[0].instructions.push_back(InitializedCheck{
        previous, InitializedSubject::binding, "deep_success"});
    chain.blocks[0].instructions.push_back(ReturnVoid{});
    deep.functions.push_back(chain);
    const auto bounded = CheckElimination::analyze(deep, 0);
    require(!bounded.ready() && bounded.report().failure == CheckGap::Budget &&
                bounded.report().certificates.empty(),
            "deep sparse SSA recursion kept a ready or partial certificate");
    require(!CheckElimination::apply(deep, bounded).applied &&
                deep.functions[0].blocks[0].instructions.size() ==
                    chain.blocks[0].instructions.size(),
            "depth-budget fallback published a partial rewrite");
    auto changed = fixture();
    const auto stale = CheckElimination::analyze(changed, 0);
    changed.functions[0].source_file = "other.qui";
    require(!CheckElimination::apply(changed, stale).applied &&
                check_at(changed.functions[0], 10),
            "source mismatch partially applied certificates");
    auto dependency = fixture();
    const auto old = CheckElimination::analyze(dependency, 0);
    auto sources = std::make_shared<facts::FactSources>();
    sources->functions["different"].result_extents.push_back(ExtentOrigin{});
    dependency.fact_sources = sources;
    require(!CheckElimination::apply(dependency, old).applied,
            "changed module fact dependencies were accepted");
    auto opaque = fixture();
    opaque.functions[0].blocks[1].instructions.insert(
        opaque.functions[0].blocks[1].instructions.begin(),
        Call{0,
             "unknown_descriptor_mutation",
             {},
             Type::simple(TypeKind::Void)});
    const auto killed = CheckElimination::analyze(opaque, 0);
    bool shape_after = false;
    for (const auto &c : killed.report().certificates)
      if (c.kind == CheckKind::ShapeConstraint && c.at.block == 1)
        shape_after = true;
    require(!shape_after,
            "opaque descriptor mutation preserved old shape proof");
    auto suffix = fixture();
    auto &first = std::get<ShapedConstraintCheck>(
        suffix.functions[0].blocks[0].instructions[4]);
    first.ellipsis = 1;
    first.extents = {1, std::nullopt, 2};
    auto &second = std::get<ShapedConstraintCheck>(
        suffix.functions[0].blocks[1].instructions[0]);
    second = first;
    const auto ellipsis = CheckElimination::analyze(suffix, 0);
    bool suffix_proven = false;
    for (const auto &c : ellipsis.report().certificates)
      if (c.kind == CheckKind::ShapeConstraint && c.at.block == 1)
        suffix_proven = true;
    require(suffix_proven,
            "unknown-rank minimum and suffix wildcard shape did not certify");
    const auto budget = CheckElimination::analyze(fixture(), 0, {0});
    require(!budget.ready() && !CheckElimination::apply(module, budget).applied,
            "exhausted budget fabricated an applicable plan");
    for (const auto mode : {OptimizerMode::Off, OptimizerMode::Verify, OptimizerMode::On}) {
      OptimizerInspection inspection;
      inspection.capture_proofs = true;
      OptimizerOptions options;
      options.check_elimination = mode;
      options.inspection = &inspection;
      const auto optimized = optimize(geometry_fixture(), options);
      bool flagged = false;
      for (const auto &block : optimized.functions[0].blocks)
        for (const auto &instruction : block.instructions)
          if (const auto *node = std::get_if<TensorCompare>(&instruction); node && node->out == 23)
            flagged = node->shape_proven;
      require(flagged == (mode == OptimizerMode::On), "check mode changed publication semantics");
      require(inspection.proofs_observed == (mode != OptimizerMode::Off), "check mode skipped or invented analysis");
      if (mode == OptimizerMode::Verify)
        for (const auto &row : inspection.proofs)
          require(!row.application_observed, "verification reported an actual application");
    }
    for (const auto mode : {OptimizerMode::Off, OptimizerMode::Verify}) {
      OptimizerInspection inspection;
      inspection.capture_proofs = true;
      OptimizerOptions options;
      options.cfg_facts = mode;
      options.check_elimination = OptimizerMode::On;
      options.inspection = &inspection;
      const auto optimized = optimize(geometry_fixture(), options);
      for (const auto &block : optimized.functions[0].blocks)
        for (const auto &instruction : block.instructions)
          if (const auto *node = std::get_if<TensorCompare>(&instruction))
            require(!node->shape_proven, "CFG disabled/verify mode published a proof flag");
      require(inspection.proofs_observed == (mode == OptimizerMode::Verify), "CFG mode ignored analysis gate");
    }
    auto invalid = fixture();
    invalid.functions[0].blocks[0].instructions.insert(invalid.functions[0].blocks[0].instructions.begin(),
                                                    Clone{42, 999, Type::simple(TypeKind::Bool)});
    const auto rejected = CheckElimination::analyze(invalid, 0);
    require(rejected.report().failure == CheckGap::InvalidSsa && rejected.report().operations > 0,
            "invalid SSA bypassed operation accounting");
    auto exact_literals = fixture();
    for (auto &block : exact_literals.functions[0].blocks)
      for (auto &instruction : block.instructions)
        if (const auto *literal = std::get_if<ConstantInt>(&instruction);
            literal && (literal->type.kind == TypeKind::Int || literal->type.kind == TypeKind::Nat))
          instruction = ConstantExact{literal->out, literal->value, literal->type};
    const auto exact_plan = CheckElimination::analyze(exact_literals, 0);
    bool exact_inline = false;
    for (const auto &certificate : exact_plan.report().certificates)
      if (const auto *binary = std::get_if<Binary>(&certificate.original))
        exact_inline |= binary->out == 34 && certificate.action == CheckAction::InlineFlag;
    require(exact_inline, "actual lowered ConstantExact integers bypassed the literal proof consumer");
    std::puts("check-elimination unit passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
#else
#include <cstdio>
int main() {
  std::fputs("missing typed check-elimination core\n", stderr);
  return 1;
}
#endif
