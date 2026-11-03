#pragma once
#include "ir/analysis/cfg.hpp"
#include "ir/facts/tensor.hpp"
#include "quidra/ir/module.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace quidra::optimizer {
enum class CheckKind : std::uint8_t {
  ShapeConstraint,
  ExtentEquality,
  Initialization,
  ForStep,
  ArrayBounds,
  ArrayInitialization,
  Overflow,
  InlineInteger,
  TensorBroadcast,
  TensorShape
};
enum class CheckAction : std::uint8_t {
  Erase,
  BoundsFlag,
  InitializationFlag,
  OverflowFlag,
  InlineFlag,
  BroadcastFlag,
  ShapeFlag
};
enum class ProofRule : std::uint8_t {
  BeforeShape,
  CanonicalExtentIdentity,
  SameSsaValue,
  LiteralTrue,
  LiteralStepSign,
  TrueGuard,
  EvaluatedIntegerRange
};
enum class CheckGap : std::uint8_t {
  None,
  Unknown,
  LiteralFailure,
  NotMaterializable,
  Unreachable,
  InvalidSsa,
  Budget,
  PreimageMismatch,
  MissingPlan
};
struct ProofPremise {
  enum class Kind : std::uint8_t {
    TensorBefore,
    ExtentBefore,
    IntegerLiteral,
    BooleanLiteral
  } kind{};
  ir::ValueId value{};
  ir::analysis::InstrRef definition;
  std::optional<ir::facts::TensorFacts> tensor;
  std::optional<ShapeExtent> extent;
  std::uint64_t magnitude{};
  bool negative{}, boolean{};
};
struct ProofCertificate {
  CheckKind kind{};
  CheckAction action{};
  ProofRule rule{};
  ir::analysis::InstrRef at;
  ir::Instruction original;
  // Complete raw authored fields, including source/diagnostic context.
  // Symbol renaming, hashes and location-neutral identity are insufficient.
  std::string original_fields;
  std::vector<ProofPremise> premises;
};
struct CheckUnavailable {
  CheckKind kind{};
  ir::analysis::InstrRef at;
  CheckGap gap{};
};
struct CheckEliminationReport {
  std::size_t removed{}, flagged{};
  std::uint64_t operations{};
  std::vector<ProofCertificate> certificates;
  std::vector<CheckUnavailable> unavailable;
  CheckGap failure{CheckGap::None};
};
struct CheckEliminationOptions {
  std::uint64_t maximum_operations{2000000};
  std::size_t maximum_preimage_bytes{64 * 1024 * 1024};
};
class CheckEliminationPlan {
public:
  bool ready() const noexcept;
  const CheckEliminationReport &report() const noexcept;

private:
  struct Detail;
  std::shared_ptr<const Detail> detail_;
  explicit CheckEliminationPlan(std::shared_ptr<const Detail> detail)
      : detail_(std::move(detail)) {}
  friend class CheckElimination;
};
struct CheckApplyResult {
  bool applied{};
  std::size_t removed{}, flagged{};
  CheckGap failure{CheckGap::None};
  std::uint64_t operations{};
};
class CheckElimination {
public:
  // Observational private core: creates a fresh exact-body AnalysisCache.
  // No pipeline/CLI activation, no module or instruction schema additions.
  static CheckEliminationPlan analyze(const ir::Module &, std::size_t function,
                                      CheckEliminationOptions = {});
  // Checks the complete original source/body/dependency payload before
  // applying to a copy, then publishes only the target Function atomically.
  static CheckApplyResult apply(ir::Module &, const CheckEliminationPlan &,
                                std::optional<std::uint64_t> maximum_operations = {});
};
} // namespace quidra::optimizer
