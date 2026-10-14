#pragma once
#include "ir/analysis/loop_invariants.hpp"
#include "quidra/ir/module.hpp"
#include <cstdint>
#include <optional>
namespace quidra::ir::analysis {
// Structural graph utility only: never an instruction eligibility certificate.
struct RegionGraph {
  std::vector<std::vector<std::uint32_t>> successors;
  std::uint32_t entry{}, exit{};
};
struct SeseSet {
  std::uint32_t entry{}, exit{};
  std::vector<std::uint32_t> members;
};
struct StructuralRegions {
  std::vector<SeseSet> regions;
  std::uint64_t operations{};
  bool budget_exhausted{};
};
StructuralRegions
structural_regions(const RegionGraph &,
                   std::uint64_t maximum_operations = 2'000'000);
enum class RegionScope : std::uint8_t { FunctionBody, LoopIteration };
enum class RegionReason : std::uint8_t {
  HostSync,
  DeviceDataRead,
  ExternalIo,
  PolicyWrite,
  OpaqueCall,
  MissingKernel,
  UnsupportedInstruction,
  TrackedAdUnavailable,
  TensorCompare,
  Parallel,
  NonKeyExit,
  NestedLoop,
  Irreducible,
  InfinitePaths,
  StateIdentityUnavailable,
  ExclusiveWindowUnavailable,
  OwnershipRoleUnavailable,
  InvalidSsa,
  BindingUnavailable,
  Budget
};
struct RegionExclusion {
  std::optional<InstrRef> at;
  RegionReason reason{};
  std::optional<std::uint32_t> loop;
};
struct RegionState {
  StorageClassId storage{};
  std::vector<StateSource> sources;
  bool read{}, written{}, identity_available{}, exclusive_window_available{};
};
struct RegionLocal {
  std::string name;
  std::vector<LocalRole> roles;
  std::optional<ReachingDef> incoming, outgoing;
  std::optional<std::uint32_t> parameter;
  std::optional<Type> type;
  std::optional<SlotFacts> facts;
};
struct StepRegion {
  std::uint32_t id{};
  RegionScope scope{};
  std::optional<std::uint32_t> loop;
  InstrRef entry;
  std::optional<InstrRef> exit;
  std::vector<InstrRef> instructions;
  std::vector<ValueId> inputs, outputs;
  std::vector<RegionLocal> locals;
  std::vector<RegionState> state;
  std::vector<std::string> operations, kernels;
  bool boundaries_complete{true}, finite_exit_only{}, internal_acyclic{true};
  // Logical only; cannot authorize motion, numerical folding or donation.
  bool physical_ownership_proven{};
};
struct StepRegionOptions {
  std::uint64_t maximum_operations{2'000'000};
};
struct StepRegionReport {
  std::vector<StepRegion> regions;
  std::vector<RegionExclusion> exclusions;
  std::uint64_t operations{};
  bool budget_exhausted{}, ssa_valid{true};
};
class StepRegionFormation {
public:
  // Fresh current-module analyses/registry; no external unbound proof flags,
  // mutation, annotation activation or invented compiled-AD capability.
  static StepRegionReport analyze(const Module &, std::size_t function,
                                  StepRegionOptions = {});
};
} // namespace quidra::ir::analysis
