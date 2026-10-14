#include "ir/analysis/step_regions.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "ir/verify.hpp"
#include "optimizer/rule_registry.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace quidra::ir::analysis {
namespace {
struct Exhausted {};
struct Work {
  std::uint64_t left{}, used{};
  void take() {
    if (!left)
      throw Exhausted{};
    --left;
    ++used;
  }
};
using Vertex = std::uint32_t;
using Site = std::pair<BlockId, std::uint32_t>;
Site key(InstrRef at) { return {at.block, at.index}; }
std::vector<std::vector<Vertex>> predecessors(const RegionGraph &graph,
                                              Work &work) {
  if (graph.entry >= graph.successors.size() ||
      graph.exit >= graph.successors.size())
    throw std::invalid_argument("invalid SESE scope endpoints");
  std::vector<std::vector<Vertex>> pred(graph.successors.size());
  for (Vertex n = 0; n < graph.successors.size(); ++n)
    for (const auto next : graph.successors[n]) {
      work.take();
      if (next >= pred.size())
        throw std::invalid_argument("invalid SESE successor");
      pred[next].push_back(n);
    }
  return pred;
}
std::optional<SeseSet> member_set(const RegionGraph &graph,
                                  const std::vector<std::vector<Vertex>> &pred,
                                  Vertex entry, Vertex exit, Work &work) {
  if (entry == exit)
    return {};
  std::vector<bool> present(graph.successors.size());
  std::deque<Vertex> queue{entry};
  present[entry] = true;
  bool sees_exit = false;
  while (!queue.empty()) {
    work.take();
    const auto node = queue.front();
    queue.pop_front();
    if (graph.successors[node].empty())
      return {};
    for (const auto next : graph.successors[node]) {
      work.take();
      if (next == exit) {
        sees_exit = true;
        continue;
      }
      if (!present[next]) {
        present[next] = true;
        queue.push_back(next);
      }
    }
  }
  if (!sees_exit)
    return {};
  SeseSet result;
  result.entry = entry;
  result.exit = exit;
  std::vector<std::uint32_t> indegree(present.size());
  for (Vertex n = 0; n < present.size(); ++n)
    if (present[n]) {
      work.take();
      result.members.push_back(n);
      for (const auto p : pred[n]) {
        work.take();
        if (present[p])
          ++indegree[n];
        else if (n != entry)
          return {};
      }
    }
  queue.clear();
  for (const auto n : result.members)
    if (!indegree[n])
      queue.push_back(n);
  std::size_t visited = 0;
  while (!queue.empty()) {
    work.take();
    const auto n = queue.front();
    queue.pop_front();
    ++visited;
    for (const auto next : graph.successors[n]) {
      work.take();
      if (present[next] && !--indegree[next])
        queue.push_back(next);
    }
  }
  // Finite-exit postdominance alone cannot justify cyclic membership.
  if (visited != result.members.size())
    return {};
  return result;
}
std::vector<SeseSet> enumerate(const RegionGraph &graph,
                               const std::vector<Vertex> &entries,
                               const std::vector<Vertex> &exits, Work &work) {
  auto pred = predecessors(graph, work);
  std::vector<SeseSet> result;
  for (const auto entry : entries)
    for (const auto exit : exits) {
      work.take();
      if (const auto set = member_set(graph, pred, entry, exit, work))
        result.push_back(*set);
    }
  return result;
}
void maximal(std::vector<SeseSet> &sets, Work &work) {
  std::vector<bool> smaller(sets.size());
  for (std::size_t a = 0; a < sets.size(); ++a)
    for (std::size_t b = 0; b < sets.size(); ++b) {
      work.take();
      if (sets[a].members.size() >= sets[b].members.size())
        continue;
      auto i = sets[b].members.begin();
      auto j = sets[a].members.begin();
      while (i != sets[b].members.end() && j != sets[a].members.end()) {
        work.take();
        if (*i < *j)
          ++i;
        else if (*i == *j) {
          ++i;
          ++j;
        } else
          break;
      }
      if (j == sets[a].members.end())
        smaller[a] = true;
    }
  std::vector<SeseSet> result;
  for (std::size_t i = 0; i < sets.size(); ++i)
    if (!smaller[i])
      result.push_back(std::move(sets[i]));
  sets = std::move(result);
}
struct Operands : OperandVisitor {
  std::vector<ValueId> definitions, uses;
  std::vector<std::pair<std::string, LocalRole>> locals;
  bool complete{true};
  void definition(ValueId value) override {
    if (value)
      definitions.push_back(value);
  }
  void use(ValueId value, OperandUse role) override {
    if (value)
      uses.push_back(value);
    complete &= role.role_available;
  }
  void local(std::string_view name, LocalRole role) override {
    locals.emplace_back(name, role);
  }
};
struct Info {
  std::vector<RegionReason> reasons;
  std::vector<ValueId> writes;
  std::vector<std::string> local_writes;
  std::vector<std::string> operations, kernels;
};
void reason(Info &info, RegionReason r) {
  if (std::find(info.reasons.begin(), info.reasons.end(), r) ==
      info.reasons.end())
    info.reasons.push_back(r);
}
std::optional<std::string> text(const CompilerExtensionTable &table,
                                std::string_view name) {
  const auto found = table.fields.find(std::string(name));
  if (found == table.fields.end())
    return {};
  if (const auto *value = std::get_if<std::string>(&found->second))
    return *value;
  return {};
}
struct ScopeGraph {
  RegionGraph graph;
  std::vector<std::optional<InstrRef>> sites;
  RegionScope scope{};
  std::optional<std::uint32_t> loop;
};
class Formation {
  const Module &module_;
  const Function &fn_;
  std::size_t function_;
  AnalysisCache cache_;
  optimizer::RuleRegistry registry_;
  const Cfg &cfg_;
  const LoopNest &nest_;
  const DefUse &defs_;
  const TensorFactAnalysis &tensors_;
  const AliasInfo &aliases_;
  const Slots &slots_;
  const LocalSsa &ssa_;
  const Liveness &live_;
  Work &work_;
  StepRegionReport report_;
  std::map<Site, Info> info_;

public:
  Formation(const Module &module, std::size_t function, Work &work)
      : module_(module), fn_(module.functions.at(function)),
        function_(function), cache_(module),
        registry_(
            optimizer::RuleRegistry::build(module, DeclarationBindingMode::On)),
        cfg_(cache_.cfg(function)), nest_(cache_.loops(function)),
        defs_(cache_.def_use(function)),
        tensors_(cache_.tensor_facts(function)),
        aliases_(cache_.aliases(function)), slots_(cache_.slots(function)),
        ssa_(cache_.local_ssa(function)), live_(cache_.liveness(function)),
        work_(work) {}
  StepRegionReport run() {
    for (BlockId b = 0; b < fn_.blocks.size(); ++b)
      for (std::uint32_t i = 0; i < fn_.blocks[b].instructions.size(); ++i) {
        work_.take();
        if (!cfg_.reachable[b])
          continue;
        const InstrRef at{b, i};
        Info info;
        std::visit([&](const auto &n) { classify(n, at, info); },
                   fn_.blocks[b].instructions[i]);
        for (const auto r : info.reasons)
          report_.exclusions.push_back({at, r, {}});
        info_.emplace(key(at), std::move(info));
      }
    scope(build({}));
    for (std::uint32_t l = 0; l < nest_.loops.size(); ++l) {
      work_.take();
      if (!nest_.loops[l].available) {
        report_.exclusions.push_back({{}, RegionReason::Irreducible, l});
        continue;
      }
      scope(build(l));
    }
    return std::move(report_);
  }

private:
  bool untracked(ValueId value, InstrRef at) const {
    return tensors_.at(at, value).tracked == facts::Tri::False;
  }
  void tensor_input(Info &info, ValueId value, InstrRef at) {
    if (!untracked(value, at))
      reason(info, RegionReason::TrackedAdUnavailable);
  }
  void classify(const TensorCreate &, InstrRef, Info &) {}
  void classify(const TensorShape &, InstrRef, Info &) {}
  void classify(const TensorDevice &, InstrRef, Info &) {}
  void classify(const TensorIsContiguous &, InstrRef, Info &) {}
  void classify(const TensorCompare &, InstrRef, Info &info) {
    reason(info, RegionReason::TensorCompare);
  }
  void classify(const TensorItem &, InstrRef, Info &info) {
    reason(info, RegionReason::DeviceDataRead);
  }
  void classify(const GpuSync &, InstrRef, Info &info) {
    reason(info, RegionReason::HostSync);
  }
  void classify(const TensorTransfer &n, InstrRef at, Info &info) {
    tensor_input(info, n.tensor, at);
    const auto source = tensors_.at(at, n.tensor);
    if (n.gpu || source.device.kind != facts::BackendKind::Cpu)
      reason(info, RegionReason::HostSync);
  }
  void classify(const TensorBinary &n, InstrRef at, Info &info) {
    if (n.left_type.kind == TypeKind::Tensor)
      tensor_input(info, n.left, at);
    if (n.right_type.kind == TypeKind::Tensor)
      tensor_input(info, n.right, at);
  }
  void classify(const TensorSet &n, InstrRef at, Info &info) {
    tensor_input(info, n.tensor, at);
    info.writes.push_back(n.tensor);
  }
  void classify(const TensorBoolReduce &, InstrRef, Info &info) {
    reason(info, RegionReason::DeviceDataRead);
  }
  void classify(const TensorTrack &n, InstrRef, Info &info) {
    if (n.mode != tensor_track_mode::untrack)
      reason(info, RegionReason::TrackedAdUnavailable);
  }
  void classify(const TensorIsTracked &, InstrRef, Info &) {}
  void classify(const TensorHasGrad &, InstrRef, Info &) {}
  void classify(const Call &n, InstrRef at, Info &info) {
    const auto *summary =
        module_.summaries ? module_.summaries->find(n.callee) : nullptr;
    if (!summary || !summary->semantic || !summary->effects_complete ||
        !summary->descriptor_complete ||
        (summary->effects & effect_bit::Opaque)) {
      reason(info, RegionReason::OpaqueCall);
      return;
    }
    if (summary->effects & effect_bit::HostSync)
      reason(info, RegionReason::HostSync);
    if (summary->effects & effect_bit::ExternalIO)
      reason(info, RegionReason::ExternalIo);
    if (summary->effects & effect_bit::WritesPolicy)
      reason(info, RegionReason::PolicyWrite);
    const auto &semantic = *summary->semantic;
    if (semantic.mutates_ambient || !semantic.reference_shared_writes.empty() ||
        !semantic.reference_shared_write_origins.empty())
      reason(info, RegionReason::StateIdentityUnavailable);
    if (summary->semantic_parameter_for_ir_parameter.size() != n.args.size()) {
      reason(info, RegionReason::OpaqueCall);
      return;
    }
    for (std::size_t p = 0; p < n.args.size(); ++p) {
      work_.take();
      const auto mapped = summary->semantic_parameter_for_ir_parameter[p];
      if (!mapped || *mapped >= semantic.exposed_writes.size()) {
        reason(info, RegionReason::OpaqueCall);
        continue;
      }
      if (!semantic.exposed_writes[*mapped].empty() ||
          (*mapped < semantic.mutates_shared_of_parameter.size() &&
           semantic.mutates_shared_of_parameter[*mapped]) ||
          (*mapped == 0 && semantic.mutates_receiver))
        info.writes.push_back(
            n.args[p].writable_address.value_or(n.args[p].value));
      const auto fact = tensors_.at(at, n.args[p].value);
      if (fact.has_type && fact.type.kind == TypeKind::Tensor)
        tensor_input(info, n.args[p].value, at);
    }
    if ((summary->effects & effect_bit::DeviceWork) ||
        n.result.kind == TypeKind::Tensor) {
      bool has_kernel = false;
      for (const auto &[reference, operation] :
           registry_.declarations().operations) {
        work_.take();
        if (std::find(operation.functions.begin(), operation.functions.end(),
                      n.callee) == operation.functions.end())
          continue;
        for (const auto &table : registry_.normalized_tables()) {
          work_.take();
          if (table.extension != operation.extension ||
              table.schema_revision != 2 || !table.normalized ||
              table.normalized->kind != CompilerExtensionTableKind::Kernel ||
              text(*table.normalized, "operation") != operation.contract.id)
            continue;
          if (text(*table.normalized, "host_sync") !=
              std::optional<std::string>{"none"})
            continue;
          has_kernel = true;
          info.operations.push_back(reference);
          info.kernels.push_back(table.extension + ":" + table.name);
        }
      }
      if (!has_kernel)
        reason(info, RegionReason::MissingKernel);
    }
  }
  void classify(const IndirectCall &, InstrRef, Info &info) {
    reason(info, RegionReason::OpaqueCall);
  }
  void classify(const ParallelBlock &, InstrRef, Info &info) {
    reason(info, RegionReason::Parallel);
  }
  void classify(const StoreLocal &n, InstrRef, Info &info) {
    info.local_writes.push_back(n.name);
    if (const auto f = slots_.facts.find(n.name);
        f == slots_.facts.end() || f->second.writable_parameter ||
        f->second.reference_bound || f->second.reference_binding ||
        f->second.address_taken || f->second.opaque_access)
      reason(info, RegionReason::StateIdentityUnavailable);
  }
  void classify(const StoreAddress &n, InstrRef, Info &info) {
    info.writes.push_back(n.address);
  }
  void classify(const StoreReference &, InstrRef, Info &info) {
    reason(info, RegionReason::StateIdentityUnavailable);
  }
  void classify(const BindReference &, InstrRef, Info &info) {
    reason(info, RegionReason::StateIdentityUnavailable);
  }
  void classify(const BindFieldReference &, InstrRef, Info &info) {
    reason(info, RegionReason::StateIdentityUnavailable);
  }
  void classify(const FieldSet &n, InstrRef, Info &info) {
    info.writes.push_back(n.object);
  }
  void classify(const ArraySet &n, InstrRef, Info &info) {
    info.writes.push_back(n.array);
  }
  void classify(const ArraySliceSet &n, InstrRef, Info &info) {
    info.writes.push_back(n.array);
  }
  void classify(const BinSet &, InstrRef, Info &info) {
    reason(info, RegionReason::StateIdentityUnavailable);
  }
  void classify(const InlineExit &, InstrRef, Info &info) {
    reason(info, RegionReason::NonKeyExit);
  }
  void classify(const Return &, InstrRef, Info &info) {
    reason(info, RegionReason::NonKeyExit);
  }
  void classify(const ReturnVoid &, InstrRef, Info &info) {
    reason(info, RegionReason::NonKeyExit);
  }
  void classify(const Exit &, InstrRef, Info &info) {
    reason(info, RegionReason::NonKeyExit);
  }
  void classify(const FailError &, InstrRef, Info &info) {
    reason(info, RegionReason::NonKeyExit);
  }
  template <class T> void classify(const T &n, InstrRef at, Info &info) {
    constexpr auto domain = InstructionTraits<T>::domain;
    if constexpr (domain == Domain::tensor) {
      if constexpr (requires { n.tensor; })
        tensor_input(info, n.tensor, at);
      else
        reason(info, RegionReason::UnsupportedInstruction);
    } else if constexpr (domain == Domain::autograd)
      reason(info, RegionReason::TrackedAdUnavailable);
    else if constexpr (domain == Domain::console || domain == Domain::file ||
                       domain == Domain::system || domain == Domain::http ||
                       domain == Domain::repl)
      reason(info, RegionReason::ExternalIo);
    else if constexpr (domain == Domain::concurrency)
      reason(info, RegionReason::Parallel);
    else if constexpr (domain == Domain::random || domain == Domain::json)
      reason(info, RegionReason::UnsupportedInstruction);
    else if constexpr (domain == Domain::call)
      reason(info, RegionReason::OpaqueCall);
    // Host scalar checks retain their actual membership/program position.
  }
  ScopeGraph build(std::optional<std::uint32_t> loop) {
    ScopeGraph scope;
    scope.scope = loop ? RegionScope::LoopIteration : RegionScope::FunctionBody;
    scope.loop = loop;
    std::set<BlockId> blocks;
    if (loop)
      blocks.insert(nest_.loops[*loop].blocks.begin(),
                    nest_.loops[*loop].blocks.end());
    else
      for (BlockId b = 0; b < fn_.blocks.size(); ++b)
        if (cfg_.reachable[b])
          blocks.insert(b);
    std::map<BlockId, Vertex> first, last;
    for (const auto b : blocks) {
      work_.take();
      first[b] = static_cast<Vertex>(scope.sites.size());
      if (fn_.blocks[b].instructions.empty())
        scope.sites.push_back({});
      else
        for (std::uint32_t i = 0; i < fn_.blocks[b].instructions.size(); ++i) {
          work_.take();
          scope.sites.push_back(InstrRef{b, i});
        }
      last[b] = static_cast<Vertex>(scope.sites.size() - 1);
    }
    const auto exit = static_cast<Vertex>(scope.sites.size());
    scope.sites.push_back({});
    scope.graph.successors.resize(scope.sites.size());
    scope.graph.entry = first.at(loop ? nest_.loops[*loop].header : BlockId{0});
    scope.graph.exit = exit;
    for (const auto b : blocks) {
      for (auto i = first[b]; i < last[b]; ++i)
        scope.graph.successors[i].push_back(i + 1);
      if (cfg_.succ[b].empty())
        scope.graph.successors[last[b]].push_back(exit);
      else
        for (const auto next : cfg_.succ[b]) {
          work_.take();
          const bool cut = loop && next == nest_.loops[*loop].header &&
                           std::find(nest_.loops[*loop].latches.begin(),
                                     nest_.loops[*loop].latches.end(),
                                     b) != nest_.loops[*loop].latches.end();
          scope.graph.successors[last[b]].push_back(
              !blocks.contains(next) || cut ? exit : first.at(next));
        }
    }
    return scope;
  }
  bool nested(const ScopeGraph &scope, InstrRef at) const {
    const auto owner = nest_.innermost_of_block[at.block];
    if (!scope.loop)
      return owner >= 0;
    return owner >= 0 && static_cast<std::uint32_t>(owner) != *scope.loop;
  }
  std::set<Site> members(const ScopeGraph &scope, const SeseSet &set) const {
    std::set<Site> result;
    for (const auto vertex : set.members)
      if (scope.sites[vertex])
        result.insert(key(*scope.sites[vertex]));
    return result;
  }
  bool state_proof(const ScopeGraph &scope, const SeseSet &set,
                   const std::set<Site> &owned,
                   std::vector<RegionState> &states) {
    std::set<StorageClassId> written;
    std::set<std::string> written_locals;
    for (const auto site : owned)
      written_locals.insert(info_.at(site).local_writes.begin(),
                            info_.at(site).local_writes.end());
    for (const auto site : owned)
      for (const auto value : info_.at(site).writes) {
        work_.take();
        const auto *storage = aliases_.find(value);
        if (!storage || !storage->storage_available || storage->classes.empty())
          return false;
        written.insert(storage->classes.begin(), storage->classes.end());
      }
    std::optional<LoopInvariants> invariant;
    const auto region_id = static_cast<std::uint32_t>(report_.regions.size());
    if ((!written.empty() || !written_locals.empty()) && scope.loop) {
      const ModuleSummaries empty;
      LoopInvariantInputs inputs{defs_,
                                 ssa_,
                                 slots_,
                                 aliases_,
                                 live_,
                                 module_.summaries ? *module_.summaries : empty,
                                 &tensors_,
                                 module_.fact_sources.get(),
                                 {{region_id, *scope.sites[set.entry], {}}}};
      for (const auto site : owned)
        inputs.regions[0].instructions.push_back({site.first, site.second});
      invariant = loop_invariants(fn_, cfg_, cache_.dominators(function_),
                                  nest_, inputs);
    }
    for (const auto &name : written_locals) {
      work_.take();
      if (!slots_.promotable(name))
        return false;
      bool declared_inside = false;
      for (const auto site : owned) {
        const auto *declaration = std::get_if<DeclareLocal>(
            &fn_.blocks[site.first].instructions[site.second]);
        declared_inside |= declaration && declaration->name == name;
      }
      // A new unexposed frame slot is internal. For an existing loop state
      // binding, use the exact writer window and derived-value backedge live
      // set; neither condition is physical ownership/donation permission.
      if (!scope.loop || declared_inside)
        continue;
      bool only_here = false;
      if (invariant)
        for (const auto &loop : invariant->loops)
          if (loop.loop == scope.loop) {
            const auto local = loop.locals.find(name);
            only_here = local != loop.locals.end() &&
                        local->second.entry_available &&
                        local->second.region_only_writes == region_id;
          }
      if (!only_here || !live_.roles_complete)
        return false;
      for (const auto latch : nest_.loops[*scope.loop].latches) {
        if (fn_.blocks[latch].instructions.empty())
          return false;
        const auto *live = live_.after(
            {latch, static_cast<std::uint32_t>(
                        fn_.blocks[latch].instructions.size() - 1)});
        if (!live)
          return false;
        for (const auto value : live->values) {
          const auto *record = defs_.find(value);
          if (!record || record->definitions.size() != 1)
            return false;
          const auto site = record->definitions.front();
          const auto *load = std::get_if<LoadLocal>(
              &fn_.blocks[site.block].instructions[site.index]);
          if (load && load->name == name && !owned.contains(key(site)))
            return false;
        }
      }
    }
    for (const auto id : written) {
      work_.take();
      const auto found =
          std::find_if(aliases_.classes.begin(), aliases_.classes.end(),
                       [&](const auto &c) { return c.id == id; });
      if (found == aliases_.classes.end())
        return false;
      RegionState state{
          id, found->state, true, true, found->state_identity_available, false};
      const bool internal =
          !found->sites.empty() &&
          std::all_of(found->sites.begin(), found->sites.end(),
                      [&](const auto at) { return owned.contains(key(at)); }) &&
          std::all_of(
              found->origins.begin(), found->origins.end(),
              [](auto origin) { return origin == StorageOrigin::Fresh; });
      if (internal)
        state.exclusive_window_available = true;
      else if (invariant)
        for (const auto &loop : invariant->loops)
          if (loop.loop == scope.loop)
            for (const auto &proof : loop.storage)
              if (proof.storage == id)
                state.exclusive_window_available =
                    state.identity_available &&
                    proof.state_identity_available &&
                    proof.region_only_writes == region_id &&
                    proof.backedge_live_available && !proof.outside_region_live;
      if (!state.identity_available || !state.exclusive_window_available)
        return false;
      states.push_back(std::move(state));
    }
    return true;
  }
  void boundaries(StepRegion &region, const std::set<Site> &owned) {
    std::set<ValueId> input, output;
    for (const auto &value : defs_.values) {
      work_.take();
      const bool defined = value.definitions.size() == 1 &&
                           owned.contains(key(value.definitions[0]));
      for (const auto &use : value.uses) {
        work_.take();
        if (owned.contains(key(use.at)) && !defined)
          input.insert(value.value);
        else if (!owned.contains(key(use.at)) && defined)
          output.insert(value.value);
        region.boundaries_complete &= use.role_available;
      }
    }
    std::map<std::string, RegionLocal, std::less<>> locals;
    for (const auto at : region.instructions) {
      Operands op;
      visit_operands(fn_.blocks[at.block].instructions[at.index], op,
                     OperandContext{&module_});
      region.boundaries_complete &= op.complete;
      for (const auto &[name, role] : op.locals) {
        auto &local = locals[name];
        local.name = name;
        if (std::find(local.roles.begin(), local.roles.end(), role) ==
            local.roles.end())
          local.roles.push_back(role);
      }
    }
    if (region.loop)
      for (const auto latch : nest_.loops[*region.loop].latches) {
        if (fn_.blocks[latch].instructions.empty()) {
          region.boundaries_complete = false;
          continue;
        }
        const auto *live = live_.after(
            {latch, static_cast<std::uint32_t>(
                        fn_.blocks[latch].instructions.size() - 1)});
        if (!live) {
          region.boundaries_complete = false;
          continue;
        }
        for (const auto value : live->values) {
          const auto *def = defs_.find(value);
          if (def && def->definitions.size() == 1 &&
              owned.contains(key(def->definitions[0])))
            output.insert(value);
        }
        region.boundaries_complete &= live->storage_available;
      }
    for (auto &[name, local] : locals) {
      local.incoming = ssa_.reaching(region.entry, name);
      if (const auto found = slots_.type.find(name); found != slots_.type.end())
        local.type = found->second;
      if (const auto found = slots_.facts.find(name);
          found != slots_.facts.end())
        local.facts = found->second;
      for (std::uint32_t parameter = 0; parameter < fn_.parameters.size();
           ++parameter)
        if (fn_.parameters[parameter].name == name)
          local.parameter = parameter;
      if (local.incoming->available &&
          local.incoming->kind == ReachingDef::Store)
        input.insert(local.incoming->value);
      if (region.exit) {
        local.outgoing = ssa_.reaching(*region.exit, name);
        if (local.outgoing->available &&
            local.outgoing->kind == ReachingDef::Store) {
          const auto *record = defs_.find(local.outgoing->value);
          if (record && record->definitions.size() == 1 &&
              owned.contains(key(record->definitions[0])))
            output.insert(local.outgoing->value);
        }
      }
      region.locals.push_back(std::move(local));
    }
    region.inputs.assign(input.begin(), input.end());
    region.outputs.assign(output.begin(), output.end());
    std::set<StorageClassId> read;
    for (const auto value : region.inputs)
      if (const auto *storage = aliases_.find(value)) {
        region.boundaries_complete &= storage->storage_available;
        read.insert(storage->classes.begin(), storage->classes.end());
      }
    for (const auto id : read)
      if (std::none_of(region.state.begin(), region.state.end(),
                       [&](const auto &s) { return s.storage == id; })) {
        const auto c =
            std::find_if(aliases_.classes.begin(), aliases_.classes.end(),
                         [&](const auto &s) { return s.id == id; });
        if (c != aliases_.classes.end() && !c->state.empty())
          region.state.push_back(
              {id, c->state, true, false, c->state_identity_available, false});
      }
  }
  void scope(ScopeGraph scope) {
    if (scope.sites.empty())
      return;
    auto pred = predecessors(scope.graph, work_);
    std::set<Vertex> entry{scope.graph.entry}, exit{scope.graph.exit};
    std::vector<bool> barred(scope.sites.size());
    for (Vertex v = 0; v < scope.sites.size(); ++v) {
      work_.take();
      if (!scope.sites[v])
        continue;
      const auto at = *scope.sites[v];
      barred[v] = nested(scope, at) || !info_.at(key(at)).reasons.empty();
      if (!nest_.loop_available[at.block]) {
        barred[v] = true;
        report_.exclusions.push_back(
            {at, RegionReason::Irreducible, scope.loop});
      }
      if (!scope.loop) {
        const auto &post = cache_.post_dominators(function_);
        if (post.idom[at.block] < 0) {
          barred[v] = true;
          report_.exclusions.push_back({at, RegionReason::InfinitePaths, {}});
        }
      }
      if (nested(scope, at))
        report_.exclusions.push_back(
            {at, RegionReason::NestedLoop, scope.loop});
      if (barred[v] || !info_.at(key(at)).writes.empty() ||
          !info_.at(key(at)).local_writes.empty()) {
        exit.insert(v);
        for (const auto next : scope.graph.successors[v])
          entry.insert(next);
      }
      if (pred[v].size() != 1 || scope.graph.successors[v].size() != 1) {
        entry.insert(v);
        exit.insert(v);
      }
      for (const auto p : pred[v])
        if (scope.graph.successors[p].size() != 1)
          entry.insert(v);
    }
    const auto sets = enumerate(scope.graph, {entry.begin(), entry.end()},
                                {exit.begin(), exit.end()}, work_);
    std::vector<SeseSet> eligible;
    for (const auto &set : sets) {
      work_.take();
      if (!scope.sites[set.entry])
        continue;
      if (std::any_of(set.members.begin(), set.members.end(),
                      [&](auto v) { return barred[v]; }))
        continue;
      const auto owned = members(scope, set);
      std::vector<RegionState> state;
      if (!state_proof(scope, set, owned, state)) {
        report_.exclusions.push_back({scope.sites[set.entry],
                                      RegionReason::ExclusiveWindowUnavailable,
                                      scope.loop});
        continue;
      }
      eligible.push_back(set);
    }
    maximal(eligible, work_);
    for (const auto &set : eligible) {
      work_.take();
      StepRegion region;
      region.id = static_cast<std::uint32_t>(report_.regions.size());
      region.scope = scope.scope;
      region.loop = scope.loop;
      region.entry = *scope.sites[set.entry];
      region.exit = scope.sites[set.exit];
      region.finite_exit_only = true;
      const auto owned = members(scope, set);
      for (const auto site : owned) {
        region.instructions.push_back({site.first, site.second});
        const auto &info = info_.at(site);
        region.operations.insert(region.operations.end(),
                                 info.operations.begin(),
                                 info.operations.end());
        region.kernels.insert(region.kernels.end(), info.kernels.begin(),
                              info.kernels.end());
      }
      if (!state_proof(scope, set, owned, region.state))
        continue;
      boundaries(region, owned);
      report_.regions.push_back(std::move(region));
    }
  }
};
} // namespace
StructuralRegions structural_regions(const RegionGraph &graph,
                                     std::uint64_t maximum) {
  StructuralRegions result;
  Work work{maximum, 0};
  try {
    (void)predecessors(graph, work);
    std::vector<bool> reachable(graph.successors.size());
    std::deque<Vertex> queue{graph.entry};
    reachable[graph.entry] = true;
    while (!queue.empty()) {
      work.take();
      const auto current = queue.front();
      queue.pop_front();
      for (const auto next : graph.successors[current]) {
        work.take();
        if (!reachable[next]) {
          reachable[next] = true;
          queue.push_back(next);
        }
      }
    }
    std::vector<Vertex> entries, exits;
    for (Vertex n = 0; n < graph.successors.size(); ++n) {
      work.take();
      if (!reachable[n])
        continue;
      if (n != graph.exit)
        entries.push_back(n);
      exits.push_back(n);
    }
    result.regions = enumerate(graph, entries, exits, work);
    maximal(result.regions, work);
  } catch (const Exhausted &) {
    result.regions.clear();
    result.budget_exhausted = true;
  }
  result.operations = work.used;
  return result;
}
StepRegionReport StepRegionFormation::analyze(const Module &module,
                                              std::size_t function,
                                              StepRegionOptions options) {
  StepRegionReport result;
  Work work{options.maximum_operations, 0};
  try {
    work.take();
    if (function >= module.functions.size()) {
      result.ssa_valid = false;
      result.exclusions.push_back({{}, RegionReason::InvalidSsa, {}});
      return result;
    }
    const auto valid = ir::verify(module.functions[function], &module);
    if (!valid.ssa_valid) {
      result.ssa_valid = false;
      result.exclusions.push_back({{}, RegionReason::InvalidSsa, {}});
      return result;
    }
    if (module.functions[function].blocks.empty())
      return result;
    result = Formation(module, function, work).run();
  } catch (const Exhausted &) {
    result = {};
    result.budget_exhausted = true;
    result.exclusions.push_back({{}, RegionReason::Budget, {}});
  }
  result.operations = work.used;
  return result;
}
} // namespace quidra::ir::analysis
