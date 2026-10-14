#include "optimizer/check_elimination.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "ir/canonical_encoding.hpp"
#include "ir/fact_sources.hpp"
#include "ir/verify.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/numeric_types.hpp"
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
namespace quidra::optimizer {
namespace {
using namespace ir;
using namespace ir::analysis;
using namespace ir::facts;
struct BudgetFailure {};
struct Work {
  std::uint64_t left{}, used{};
  void take() {
    if (!left)
      throw BudgetFailure{};
    --left;
    ++used;
  }
};
// Exact raw authored fields. This deliberately does not call the canonical
// symbol/location normalizer: a plan belongs to one original preimage.
class ExactFields {
public:
  ExactFields(Work &work, std::size_t limit) : work_(work), limit_(limit) {}
  std::string finish() { return std::move(bytes_); }
  template <class T> void put(const T &value) {
    using V = std::remove_cvref_t<T>;
    work_.take();
    if (depth_ >= 256)
      throw BudgetFailure{};
    struct Depth {
      std::size_t &n;
      Depth(std::size_t &x) : n(x) { ++n; }
      ~Depth() { --n; }
    } depth(depth_);
    using namespace ir::canonical_detail;
    if constexpr (std::is_same_v<V, bool>)
      word(value ? 1 : 0);
    else if constexpr (std::is_enum_v<V>)
      put(static_cast<std::underlying_type_t<V>>(value));
    else if constexpr (std::is_integral_v<V>) {
      static_assert(sizeof(V) <= 8);
      word(static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_same_v<V, float>)
      word(std::bit_cast<std::uint32_t>(value));
    else if constexpr (std::is_same_v<V, double>)
      word(std::bit_cast<std::uint64_t>(value));
    else if constexpr (std::is_same_v<V, std::string> ||
                       std::is_same_v<V, std::string_view>) {
      word(value.size());
      append(value);
    } else if constexpr (std::is_same_v<V, ShapeCoefficient>)
      put(value.text());
    else if constexpr (IsOptional<V>::value) {
      word(value ? 1 : 0);
      if (value)
        put(*value);
    } else if constexpr (IsVariant<V>::value) {
      if (value.valueless_by_exception())
        throw std::invalid_argument("valueless check preimage");
      word(value.index());
      std::visit([&](const auto &node) { put(node); }, value);
    } else if constexpr (IsShared<V>::value) {
      word(value ? 1 : 0);
      if (value) {
        if (std::find(active_.begin(), active_.end(), value.get()) !=
            active_.end())
          throw std::invalid_argument("cyclic check preimage");
        active_.push_back(value.get());
        put(*value);
        active_.pop_back();
      }
    } else if constexpr (IsVector<V>::value || IsArray<V>::value ||
                         IsSet<V>::value) {
      word(value.size());
      for (const auto &item : value) {
        if constexpr (std::is_same_v<typename V::value_type, bool>)
          put(static_cast<bool>(item));
        else
          put(item);
      }
    } else if constexpr (IsMap<V>::value) {
      word(value.size());
      for (const auto &[key, item] : value) {
        put(key);
        put(item);
      }
    } else if constexpr (IsPair<V>::value) {
      put(value.first);
      put(value.second);
    } else if constexpr (std::is_aggregate_v<V> || std::is_same_v<V, Binary> ||
                         std::is_same_v<V, ShapeExtent>) {
      word(ir::fields::aggregate_arity<V>());
      ir::fields::for_each_field(value,
                                 [&](auto, const auto &field) { put(field); });
    } else
      static_assert(ir::canonical_detail::unsupported<V>,
                    "complete raw check preimage needs an authored adapter");
  }

private:
  Work &work_;
  std::size_t limit_, depth_{};
  std::string bytes_;
  std::vector<const void *> active_;
  void append(std::string_view text) {
    if (bytes_.size() > limit_ || text.size() > limit_ - bytes_.size())
      throw BudgetFailure{};
    bytes_.append(text);
  }
  void word(std::uint64_t value) {
    if (bytes_.size() > limit_ || 8 > limit_ - bytes_.size())
      throw BudgetFailure{};
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes_.push_back(static_cast<char>(value >> shift));
  }
};
std::string dependencies(const Module &module, Work &work,
                         std::size_t maximum) {
  ExactFields fields(work, maximum);
  fields.put(module.functions);
  fields.put(module.classes);
  fields.put(static_cast<bool>(module.summaries));
  if (module.summaries) {
    fields.put(module.summaries->by_function.size());
    for (const auto &[name, row] : module.summaries->by_function) {
      fields.put(name);
      fields.put(static_cast<const IrSummaryFacts &>(row));
      fields.put(row.semantic_key);
      fields.put(row.semantic_parameter_for_ir_parameter);
      fields.put(row.pure_tensor);
      fields.put(row.semantic != nullptr);
      if (row.semantic)
        fields.put(*row.semantic);
    }
  }
  fields.put(module.fact_sources);
  return fields.finish();
}
std::string original_fields(const Instruction &instruction, Work &work,
                            std::size_t maximum) {
  ExactFields fields(work, maximum);
  fields.put(instruction);
  return fields.finish();
}
struct Literal {
  Type type;
  bool is_bool{}, boolean{}, negative{};
  std::uint64_t magnitude{};
  bool same(const Literal &other) const {
    return is_bool == other.is_bool &&
           (is_bool
                ? boolean == other.boolean
                : negative == other.negative && magnitude == other.magnitude);
  }
};
bool integer_type(const Type &type) {
  const auto *info = numeric_kind_info(type.kind);
  return info && (info->family == NumericFamily::Natural ||
                  info->family == NumericFamily::Integer);
}
bool fits(const Literal &value, const Type &type, bool inline_word = false) {
  if (value.is_bool)
    return type.kind == TypeKind::Bool;
  const auto *info = numeric_kind_info(type.kind);
  if (!info || !integer_type(type))
    return false;
  if (info->family == NumericFamily::Natural && value.negative)
    return false;
  if (inline_word) {
    const auto bound =
        value.negative
            ? static_cast<std::uint64_t>(
                  -(abi::bare_integer_layout::inline_min + 1)) +
                  1
            : static_cast<std::uint64_t>(abi::bare_integer_layout::inline_max);
    return value.magnitude <= bound;
  }
  if (info->width == 0)
    return true;
  if (info->is_signed) {
    const auto negative = std::uint64_t{1} << (info->width - 1);
    return value.magnitude <= (value.negative ? negative : negative - 1);
  }
  const auto maximum = info->width == 64
                           ? std::numeric_limits<std::uint64_t>::max()
                           : (std::uint64_t{1} << info->width) - 1;
  return !value.negative && value.magnitude <= maximum;
}
std::optional<Literal> parse(const ConstantInt &n) {
  Literal result;
  result.type = n.type;
  if (!integer_type(n.type) || n.value.empty())
    return {};
  std::string_view text = n.value;
  if (text.front() == '-' || text.front() == '+') {
    result.negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.empty())
    return {};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result.magnitude);
  if (error != std::errc{} || end != text.data() + text.size())
    return {};
  if (!result.magnitude)
    result.negative = false;
  if (!fits(result, n.type))
    return {};
  return result;
}
std::optional<Literal> arithmetic(std::string_view op, Literal a, Literal b) {
  if (a.is_bool || b.is_bool)
    return {};
  Literal out;
  out.type = a.type;
  if (op == "-") {
    if (b.magnitude)
      b.negative = !b.negative;
    op = "+";
  }
  if (op == "+") {
    if (a.negative == b.negative) {
      if (b.magnitude > std::numeric_limits<std::uint64_t>::max() - a.magnitude)
        return {};
      out.magnitude = a.magnitude + b.magnitude;
      out.negative = a.negative;
    } else if (a.magnitude >= b.magnitude) {
      out.magnitude = a.magnitude - b.magnitude;
      out.negative = a.negative;
    } else {
      out.magnitude = b.magnitude - a.magnitude;
      out.negative = b.negative;
    }
  } else if (op == "*") {
    if (a.magnitude &&
        b.magnitude > std::numeric_limits<std::uint64_t>::max() / a.magnitude)
      return {};
    out.magnitude = a.magnitude * b.magnitude;
    out.negative = a.negative != b.negative;
  } else
    return {};
  if (!out.magnitude)
    out.negative = false;
  return out;
}
int compare(const Literal &a, const Literal &b) {
  if (a.negative != b.negative)
    return a.negative ? -1 : 1;
  const auto order = a.magnitude < b.magnitude   ? -1
                     : a.magnitude > b.magnitude ? 1
                                                 : 0;
  return a.negative ? -order : order;
}
bool comparison(std::string_view op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
         op == ">=";
}
bool relation(std::string_view op, int c) {
  return op == "=="   ? c == 0
         : op == "!=" ? c != 0
         : op == "<"  ? c < 0
         : op == "<=" ? c <= 0
         : op == ">"  ? c > 0
                      : c >= 0;
}
class Engine {
public:
  Engine(const Module &module, std::size_t function, Work &work,
         CheckEliminationOptions options)
      : fn_(module.functions.at(function)), cache_(module),
        cfg_(cache_.cfg(function)), dom_(cache_.dominators(function)),
        defs_(cache_.def_use(function)), ssa_(cache_.local_ssa(function)),
        tensor_(cache_.tensor_facts(function)), work_(work), options_(options) {
  }
  CheckEliminationReport run() {
    for (BlockId b = 0; b < fn_.blocks.size(); ++b)
      for (std::uint32_t i = 0; i < fn_.blocks[b].instructions.size(); ++i) {
        work_.take();
        if (!cfg_.reachable[b])
          continue;
        site_ = {b, i};
        const auto &ins = fn_.blocks[b].instructions[i];
        std::visit([&](const auto &n) { inspect(n, ins); }, ins);
      }
    return std::move(report_);
  }

private:
  const Function &fn_;
  AnalysisCache cache_;
  const Cfg &cfg_;
  const DomTree &dom_;
  const DefUse &defs_;
  const LocalSsa &ssa_;
  const TensorFactAnalysis &tensor_;
  Work &work_;
  CheckEliminationOptions options_;
  InstrRef site_;
  CheckEliminationReport report_;
  std::set<std::uint32_t> active_phis_;
  std::size_t query_depth_{};
  struct QueryDepth {
    std::size_t &depth;
    explicit QueryDepth(std::size_t &value) : depth(value) {
      if (depth >= 256)
        throw BudgetFailure{};
      ++depth;
    }
    ~QueryDepth() { --depth; }
  };
  std::optional<InstrRef> definition(ValueId value, InstrRef at) const {
    const auto *record = defs_.find(value);
    if (!value || !record || record->definitions.size() != 1)
      return {};
    const auto def = record->definitions[0];
    if (def == at || !dom_.dominates(def, at))
      return {};
    return def;
  }
  const Instruction &node(InstrRef at) const {
    return fn_.blocks.at(at.block).instructions.at(at.index);
  }
  std::optional<Literal> reaching(const ReachingDef &source,
                                  std::set<ValueId> &active) {
    QueryDepth depth(query_depth_);
    work_.take();
    if (!source.available)
      return {};
    if (source.kind == ReachingDef::Store)
      return literal(source.value, source.store, active);
    if (source.kind != ReachingDef::Phi || source.phi.value >= ssa_.phis.size())
      return {};
    if (!active_phis_.insert(source.phi.value).second)
      return {};
    struct ErasePhi {
      std::set<std::uint32_t> &set;
      std::uint32_t value;
      ~ErasePhi() { set.erase(value); }
    } erase_phi{active_phis_, source.phi.value};
    const auto &phi = ssa_.phis[source.phi.value];
    std::optional<Literal> result;
    const auto meet = [&](const ReachingDef &input) {
      auto value = reaching(input, active);
      if (!value || (result && !result->same(*value)))
        return false;
      if (!result)
        result = value;
      return true;
    };
    if (phi.entry && !meet(*phi.entry))
      return {};
    for (const auto &[pred, input] : phi.incoming) {
      (void)pred;
      if (!meet(input))
        return {};
    }
    return result;
  }
  std::optional<Literal> literal(ValueId value, InstrRef at,
                                 std::set<ValueId> &active) {
    QueryDepth depth(query_depth_);
    work_.take();
    const auto def = definition(value, at);
    if (!def || !active.insert(value).second)
      return {};
    struct Erase {
      std::set<ValueId> &set;
      ValueId value;
      ~Erase() { set.erase(value); }
    } erase{active, value};
    const auto &ins = node(*def);
    std::optional<Literal> out;
    if (const auto *n = std::get_if<ConstantInt>(&ins))
      out = parse(*n);
    else if (const auto *n = std::get_if<ConstantExact>(&ins)) {
      if (n->type.kind == TypeKind::Int || n->type.kind == TypeKind::Nat)
        out = parse(ConstantInt{n->out, n->spelling, n->type});
    }
    else if (const auto *n = std::get_if<ConstantBool>(&ins)) {
      Literal v;
      v.type = Type::simple(TypeKind::Bool);
      v.is_bool = true;
      v.boolean = n->value;
      out = v;
    } else if (const auto *n = std::get_if<LoadLocal>(&ins)) {
      if (const auto p = ssa_.load_source.find(n->out);
          p != ssa_.load_source.end())
        out = reaching(p->second, active);
      if (out && out->type != n->type)
        out.reset();
    } else if (const auto *n = std::get_if<Clone>(&ins))
      out = literal(n->value, *def, active);
    else if (const auto *n = std::get_if<NumericConvert>(&ins)) {
      out = literal(n->value, *def, active);
      if (out && out->type == n->source_type && fits(*out, n->target_type))
        out->type = n->target_type;
      else
        out.reset();
    } else if (const auto *n = std::get_if<Unary>(&ins)) {
      out = literal(n->operand, *def, active);
      if (out && n->op == "-" && !out->is_bool && out->type == n->type) {
        if (out->magnitude)
          out->negative = !out->negative;
        if (!fits(*out, n->type))
          out.reset();
      } else if (out && n->op == "not" && out->is_bool)
        out->boolean = !out->boolean;
      else
        out.reset();
    } else if (const auto *n = std::get_if<Binary>(&ins)) {
      auto a = literal(n->left, *def, active),
           b = literal(n->right, *def, active);
      if (a && b && a->type == n->operand_type && b->type == n->operand_type) {
        if (comparison(n->op) && !a->is_bool && !b->is_bool) {
          Literal v;
          v.type = Type::simple(TypeKind::Bool);
          v.is_bool = true;
          v.boolean = relation(n->op, compare(*a, *b));
          out = v;
        } else {
          out = arithmetic(n->op, *a, *b);
          if (out && fits(*out, n->result_type))
            out->type = n->result_type;
          else
            out.reset();
        }
      }
    }
    return out;
  }
  std::optional<Literal> literal(ValueId value) {
    std::set<ValueId> active;
    return literal(value, site_, active);
  }
  void premise(std::vector<ProofPremise> &list, ValueId value,
               const Literal &lit) {
    const auto def = definition(value, site_);
    if (!def)
      return;
    ProofPremise p;
    p.kind = lit.is_bool ? ProofPremise::Kind::BooleanLiteral
                         : ProofPremise::Kind::IntegerLiteral;
    p.value = value;
    p.definition = *def;
    p.magnitude = lit.magnitude;
    p.negative = lit.negative;
    p.boolean = lit.boolean;
    list.push_back(std::move(p));
  }
  void scalar_premise(std::vector<ProofPremise> &list, ValueId value,
                      const ShapeExtent &extent) {
    const auto def = definition(value, site_);
    if (!def)
      return;
    ProofPremise p;
    p.kind = ProofPremise::Kind::ExtentBefore;
    p.value = value;
    p.definition = *def;
    p.extent = extent;
    list.push_back(std::move(p));
  }
  void tensor_premise(std::vector<ProofPremise> &list, ValueId value,
                      const TensorFacts &fact) {
    const auto def = definition(value, site_);
    if (!def)
      return;
    ProofPremise p;
    p.kind = ProofPremise::Kind::TensorBefore;
    p.value = value;
    p.definition = *def;
    p.tensor = fact;
    list.push_back(std::move(p));
  }
  void certify(CheckKind kind, CheckAction action, ProofRule rule,
               const Instruction &ins, std::vector<ProofPremise> premises) {
    report_.certificates.push_back(
        {kind, action, rule, site_, ins,
         original_fields(ins, work_, options_.maximum_preimage_bytes),
         std::move(premises)});
    if (action == CheckAction::Erase)
      ++report_.removed;
    else
      ++report_.flagged;
  }
  void unavailable(CheckKind kind, CheckGap gap = CheckGap::Unknown) {
    report_.unavailable.push_back({kind, site_, gap});
  }
  std::optional<ShapeExtent> scalar(ValueId value) {
    if (!definition(value, site_))
      return {};
    auto extent = tensor_.scalar_at(site_, value);
    if (extent.kind == ShapeExtent::Kind::Unknown)
      return {};
    return extent;
  }
  void inspect(const ShapedConstraintCheck &n, const Instruction &ins) {
    if (n.kind != TypeKind::Tensor ||
        (n.ellipsis && *n.ellipsis > n.extents.size())) {
      unavailable(CheckKind::ShapeConstraint);
      return;
    }
    const auto fact = tensor_.at(site_, n.value);
    const auto rank = fact.rank_extent.known();
    bool proven = fact.has_type && fact.type.kind == TypeKind::Tensor;
    if (n.ellipsis)
      proven &=
          (rank && static_cast<std::uint64_t>(*rank) >= n.extents.size()) ||
          fact.minimum_rank >= n.extents.size();
    else
      proven &= rank && static_cast<std::uint64_t>(*rank) == n.extents.size();
    std::vector<ProofPremise> premises;
    tensor_premise(premises, n.value, fact);
    for (std::size_t axis = 0; proven && axis < n.extents.size(); ++axis) {
      if (!n.extents[axis])
        continue;
      const auto expected = scalar(*n.extents[axis]);
      ShapeExtent actual;
      if (rank) {
        const auto physical =
            n.ellipsis && axis >= *n.ellipsis
                ? static_cast<std::size_t>(*rank) - (n.extents.size() - axis)
                : axis;
        if (physical < fact.extents.size())
          actual = fact.extents[physical];
      } else if (axis < *n.ellipsis) {
        if (axis < fact.head_extents.size())
          actual = fact.head_extents[axis];
      } else {
        const auto distance = n.extents.size() - axis;
        if (distance <= fact.tail_extents.size())
          actual = fact.tail_extents[fact.tail_extents.size() - distance];
      }
      proven = expected && same_extent(actual, *expected);
      if (expected)
        scalar_premise(premises, *n.extents[axis], *expected);
    }
    if (proven)
      certify(CheckKind::ShapeConstraint, CheckAction::Erase,
              ProofRule::BeforeShape, ins, std::move(premises));
    else
      unavailable(CheckKind::ShapeConstraint);
  }
  void inspect(const ExtentEqualCheck &n, const Instruction &ins) {
    const auto a = scalar(n.actual), b = scalar(n.expected);
    std::vector<ProofPremise> premises;
    if (n.actual == n.expected && definition(n.actual, site_)) {
      if (a)
        scalar_premise(premises, n.actual, *a);
      certify(CheckKind::ExtentEquality, CheckAction::Erase,
              ProofRule::SameSsaValue, ins, std::move(premises));
    } else if (a && b && same_extent(*a, *b)) {
      scalar_premise(premises, n.actual, *a);
      scalar_premise(premises, n.expected, *b);
      certify(CheckKind::ExtentEquality, CheckAction::Erase,
              ProofRule::CanonicalExtentIdentity, ins, std::move(premises));
    } else
      unavailable(CheckKind::ExtentEquality, a && b && a->known() && b->known()
                                                 ? CheckGap::LiteralFailure
                                                 : CheckGap::Unknown);
  }
  void inspect(const InitializedCheck &n, const Instruction &ins) {
    const auto flag = literal(n.flag);
    if (flag && flag->is_bool && flag->boolean) {
      std::vector<ProofPremise> p;
      premise(p, n.flag, *flag);
      certify(CheckKind::Initialization, CheckAction::Erase,
              ProofRule::LiteralTrue, ins, std::move(p));
    } else
      unavailable(CheckKind::Initialization, flag && flag->is_bool
                                                 ? CheckGap::LiteralFailure
                                                 : CheckGap::Unknown);
  }
  void inspect(const ForStepCheck &n, const Instruction &ins) {
    const auto step = literal(n.step);
    if (step && !step->is_bool && is_bare_integer(step->type) &&
        step->magnitude && step->negative == n.descending) {
      std::vector<ProofPremise> p;
      premise(p, n.step, *step);
      certify(CheckKind::ForStep, CheckAction::Erase,
              ProofRule::LiteralStepSign, ins, std::move(p));
    } else
      unavailable(CheckKind::ForStep,
                  step ? CheckGap::LiteralFailure : CheckGap::Unknown);
  }
  template <class T> void array_flags(const T &n, const Instruction &ins) {
    const auto guard = [&](const std::optional<ValueId> &value, CheckKind kind,
                           CheckAction action) {
      if (value) {
        const auto flag = literal(*value);
        if (flag && flag->is_bool && flag->boolean) {
          std::vector<ProofPremise> p;
          premise(p, *value, *flag);
          certify(kind, action, ProofRule::TrueGuard, ins, std::move(p));
          return;
        }
      }
      unavailable(kind);
    };
    if (!n.bounds_proven)
      guard(n.bounds_guard, CheckKind::ArrayBounds, CheckAction::BoundsFlag);
    if (!n.initialization_proven)
      guard(n.initialization_guard, CheckKind::ArrayInitialization,
            CheckAction::InitializationFlag);
  }
  void inspect(const ArrayGet &n, const Instruction &ins) {
    array_flags(n, ins);
  }
  void inspect(const ArraySet &n, const Instruction &ins) {
    array_flags(n, ins);
  }
  void inspect(const Binary &n, const Instruction &ins) {
    if (!integer_type(n.operand_type))
      return;
    const auto a = literal(n.left), b = literal(n.right);
    if (!a || !b || a->is_bool || b->is_bool || a->type != n.operand_type ||
        b->type != n.operand_type) {
      unavailable(is_bare_integer(n.operand_type) ? CheckKind::InlineInteger
                                                  : CheckKind::Overflow);
      return;
    }
    const auto result = arithmetic(n.op, *a, *b);
    std::vector<ProofPremise> p;
    premise(p, n.left, *a);
    premise(p, n.right, *b);
    if (is_fixed_integer(n.operand_type) && !n.overflow_proven &&
        (n.op == "+" || n.op == "-" || n.op == "*")) {
      if (result && fits(*result, n.operand_type))
        certify(CheckKind::Overflow, CheckAction::OverflowFlag,
                ProofRule::EvaluatedIntegerRange, ins, p);
      else
        unavailable(CheckKind::Overflow, CheckGap::LiteralFailure);
    }
    if (is_bare_integer(n.operand_type) && !n.inline_proven) {
      const bool operands =
          fits(*a, n.operand_type, true) && fits(*b, n.operand_type, true);
      if (operands &&
          (comparison(n.op) || (result && fits(*result, n.result_type, true))))
        certify(CheckKind::InlineInteger, CheckAction::InlineFlag,
                ProofRule::EvaluatedIntegerRange, ins, std::move(p));
      else
        unavailable(CheckKind::InlineInteger);
    }
  }
  void inspect(const Unary &n, const Instruction &ins) {
    if (n.type.kind != TypeKind::Int || n.inline_proven || n.op != "-")
      return;
    const auto value = literal(n.operand);
    if (value && !value->is_bool && value->type == n.type) {
      auto result = *value;
      if (result.magnitude)
        result.negative = !result.negative;
      if (fits(*value, n.type, true) && fits(result, n.type, true)) {
        std::vector<ProofPremise> p;
        premise(p, n.operand, *value);
        certify(CheckKind::InlineInteger, CheckAction::InlineFlag,
                ProofRule::EvaluatedIntegerRange, ins, std::move(p));
        return;
      }
    }
    unavailable(CheckKind::InlineInteger);
  }
  void inspect(const NumericConvert &n, const Instruction &ins) {
    if (n.inline_proven ||
        !((is_fixed_integer(n.source_type) && is_bare_integer(n.target_type)) ||
          (is_bare_integer(n.source_type) && is_fixed_integer(n.target_type))))
      return;
    const auto value = literal(n.value);
    const auto bare =
        is_bare_integer(n.source_type) ? n.source_type : n.target_type;
    if (value && !value->is_bool && value->type == n.source_type &&
        fits(*value, bare, true) && fits(*value, n.target_type)) {
      std::vector<ProofPremise> p;
      premise(p, n.value, *value);
      certify(CheckKind::InlineInteger, CheckAction::InlineFlag,
              ProofRule::EvaluatedIntegerRange, ins, std::move(p));
    } else
      unavailable(CheckKind::InlineInteger);
  }
  template <class T>
  void tensor_validation(const T &n, bool broadcast, bool already_proven,
                         const Instruction &ins) {
    if (already_proven || n.left_type.kind != TypeKind::Tensor ||
        n.right_type.kind != TypeKind::Tensor)
      return;
    const auto kind =
        broadcast ? CheckKind::TensorBroadcast : CheckKind::TensorShape;
    if (!definition(n.left, site_) || !definition(n.right, site_)) {
      unavailable(kind);
      return;
    }
    const auto a = tensor_.at(site_, n.left), b = tensor_.at(site_, n.right);
    if (!a.has_type || a.type.kind != TypeKind::Tensor || !b.has_type ||
        b.type.kind != TypeKind::Tensor) {
      unavailable(kind);
      return;
    }
    const bool same_value = n.left != 0 && n.left == n.right;
    const auto ar = a.rank_extent.known(), br = b.rank_extent.known();
    bool proven =
        same_value || (ar && br && *ar >= 0 && *br >= 0 && *ar == *br &&
                       static_cast<std::uint64_t>(*ar) == a.extents.size() &&
                       b.extents.size() == a.extents.size());
    for (std::size_t i = 0; proven && !same_value && i < a.extents.size();
         ++i) {
      work_.take();
      proven = same_extent(a.extents[i], b.extents[i]) ||
               (broadcast &&
                (a.extents[i].known() == 1 || b.extents[i].known() == 1));
    }
    if (!proven) {
      unavailable(kind);
      return;
    }
    std::vector<ProofPremise> premises;
    tensor_premise(premises, n.left, a);
    tensor_premise(premises, n.right, b);
    certify(kind,
            broadcast ? CheckAction::BroadcastFlag : CheckAction::ShapeFlag,
            same_value ? ProofRule::SameSsaValue : ProofRule::BeforeShape, ins,
            std::move(premises));
  }
  void inspect(const TensorBinary &n, const Instruction &ins) {
    tensor_validation(n, true, n.broadcast_proven, ins);
  }
  void inspect(const TensorCompare &n, const Instruction &ins) {
    tensor_validation(n, false, n.shape_proven, ins);
  }
  void inspect(const AddressElement &, const Instruction &) {
    unavailable(CheckKind::ArrayBounds);
  }
  void inspect(const BinGet &, const Instruction &) {
    unavailable(CheckKind::ArrayBounds);
  }
  void inspect(const BinSet &, const Instruction &) {
    unavailable(CheckKind::ArrayBounds);
  }
  void inspect(const ArraySlice &, const Instruction &) {
    unavailable(CheckKind::ArrayBounds);
    unavailable(CheckKind::ArrayInitialization);
  }
  void inspect(const ArraySliceSet &, const Instruction &) {
    unavailable(CheckKind::ArrayBounds);
  }
  template <class T> void inspect(const T &, const Instruction &) {}
};
} // namespace
struct CheckEliminationPlan::Detail {
  std::size_t function{};
  CheckEliminationOptions options;
  std::string dependencies;
  CheckEliminationReport report;
  bool ready{};
};
bool CheckEliminationPlan::ready() const noexcept {
  return detail_ && detail_->ready;
}
const CheckEliminationReport &CheckEliminationPlan::report() const noexcept {
  static const CheckEliminationReport empty;
  return detail_ ? detail_->report : empty;
}
CheckEliminationPlan
CheckElimination::analyze(const Module &module, std::size_t function,
                          CheckEliminationOptions options) {
  auto detail = std::make_shared<CheckEliminationPlan::Detail>();
  detail->function = function;
  detail->options = options;
  Work work{options.maximum_operations, 0};
  try {
    if (function >= module.functions.size()) {
      detail->report.failure = CheckGap::MissingPlan;
      return CheckEliminationPlan(detail);
    }
    work.take();
    const auto validity = ir::verify(module.functions[function], &module);
    if (!validity.ssa_valid) {
      detail->report.failure = CheckGap::InvalidSsa;
      detail->report.operations = work.used;
      return CheckEliminationPlan(detail);
    }
    detail->dependencies =
        dependencies(module, work, options.maximum_preimage_bytes);
    detail->report = Engine(module, function, work, options).run();
    detail->ready = true;
  } catch (const BudgetFailure &) {
    detail->report = {};
    detail->report.failure = CheckGap::Budget;
  }
  detail->report.operations = work.used;
  return CheckEliminationPlan(std::move(detail));
}
CheckApplyResult CheckElimination::apply(Module &module,
                                         const CheckEliminationPlan &plan,
                                         std::optional<std::uint64_t> maximum_operations) {
  if (!plan.ready())
    return {false, 0, 0,
            plan.detail_ ? plan.detail_->report.failure
                         : CheckGap::MissingPlan};
  const auto &detail = *plan.detail_;
  Work work{std::min(detail.options.maximum_operations,
                    maximum_operations.value_or(detail.options.maximum_operations)), 0};
  try {
    if (dependencies(module, work, detail.options.maximum_preimage_bytes) !=
        detail.dependencies)
      return {false, 0, 0, CheckGap::PreimageMismatch, work.used};
    auto output = module.functions.at(detail.function);
    std::vector<std::vector<bool>> erased;
    for (const auto &block : output.blocks)
      erased.emplace_back(block.instructions.size(), false);
    for (const auto &proof : detail.report.certificates) {
      work.take();
      auto &instruction =
          output.blocks.at(proof.at.block).instructions.at(proof.at.index);
      if (original_fields(module.functions[detail.function]
                              .blocks[proof.at.block]
                              .instructions[proof.at.index],
                          work, detail.options.maximum_preimage_bytes) !=
          proof.original_fields)
        return {false, 0, 0, CheckGap::PreimageMismatch, work.used};
      if (proof.action == CheckAction::Erase) {
        erased[proof.at.block][proof.at.index] = true;
        continue;
      }
      bool accepted = false;
      std::visit(
          [&](auto &n) {
            using T = std::remove_cvref_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ArrayGet> ||
                          std::is_same_v<T, ArraySet>) {
              if (proof.action == CheckAction::BoundsFlag) {
                n.bounds_proven = true;
                accepted = true;
              } else if (proof.action == CheckAction::InitializationFlag) {
                n.initialization_proven = true;
                accepted = true;
              }
            } else if constexpr (std::is_same_v<T, Binary>) {
              if (proof.action == CheckAction::OverflowFlag) {
                n.overflow_proven = true;
                accepted = true;
              } else if (proof.action == CheckAction::InlineFlag) {
                n.inline_proven = true;
                accepted = true;
              }
            } else if constexpr (std::is_same_v<T, TensorBinary>) {
              if (proof.action == CheckAction::BroadcastFlag &&
                  proof.kind == CheckKind::TensorBroadcast) {
                n.broadcast_proven = true;
                accepted = true;
              }
            } else if constexpr (std::is_same_v<T, TensorCompare>) {
              if (proof.action == CheckAction::ShapeFlag &&
                  proof.kind == CheckKind::TensorShape) {
                n.shape_proven = true;
                accepted = true;
              }
            } else if constexpr (std::is_same_v<T, Unary> ||
                                 std::is_same_v<T, NumericConvert>) {
              if (proof.action == CheckAction::InlineFlag) {
                n.inline_proven = true;
                accepted = true;
              }
            }
          },
          instruction);
      if (!accepted)
        return {false, 0, 0, CheckGap::PreimageMismatch, work.used};
    }
    for (std::size_t b = 0; b < output.blocks.size(); ++b) {
      std::vector<Instruction> instructions;
      instructions.reserve(output.blocks[b].instructions.size());
      for (std::size_t i = 0; i < output.blocks[b].instructions.size(); ++i) {
        work.take();
        if (!erased[b][i])
          instructions.push_back(std::move(output.blocks[b].instructions[i]));
      }
      output.blocks[b].instructions = std::move(instructions);
    }
    module.functions[detail.function] = std::move(output);
    return {true, detail.report.removed, detail.report.flagged, CheckGap::None, work.used};
  } catch (const BudgetFailure &) {
    return {false, 0, 0, CheckGap::Budget, work.used};
  }
}
} // namespace quidra::optimizer
