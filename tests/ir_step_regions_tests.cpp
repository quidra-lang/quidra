#if __has_include("ir/analysis/step_regions.hpp")
#include "ir/analysis/step_regions.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
static void require(bool value, const char *why) {
  if (!value)
    throw std::runtime_error(why);
}
static Module fixture() {
  const auto natural = Type::simple(TypeKind::Nat64),
             integer = Type::simple(TypeKind::Int64),
             tensor = Type::tensor(Type::simple(TypeKind::Real32), 1);
  const auto print_result = Type::union_of(
      {Type::simple(TypeKind::Void), Type::simple(TypeKind::Error)});
  Function fn;
  fn.name = "generic_multiblock";
  fn.blocks = {
      {"entry",
       {ConstantInt{1, "4", natural},
        ArrayMake{2, {1}, Type::array(natural, 1)},
        TensorCreate{3, 2, {}, tensor, tensor_fill_mode::zeros},
        ConstantBool{4, true}, Branch{4, "left", "right"}}},
      {"left",
       {TensorContiguous{5, 3, tensor},
        ConstantInt{10, "9223372036854775807", integer},
        ConstantInt{11, "1", integer},
        Binary{12, "+", 10, 11, integer, integer, 8, 1}, Jump{"join"}}},
      {"right",
       {ConstantInt{13, "0", integer}, TensorTranspose{6, 3, 13, 13, tensor},
        Jump{"join"}}},
      {"join",
       {TensorBinary{7, "+", 3, 3, tensor, tensor, tensor},
        Print{4, Type::simple(TypeKind::Bool), 14, print_result},
        TensorItem{8, 7, Type::simple(TypeKind::Real32)}, ReturnVoid{}}}};
  Module module;
  module.functions = {fn};
  return module;
}
int main() {
  try {
    RegionGraph diamond{{{1}, {2, 3}, {4}, {4}, {5}, {}}, 0, 5};
    const auto pure = structural_regions(diamond);
    bool whole = false;
    for (const auto &r : pure.regions)
      if (r.entry == 0 && r.exit == 5 &&
          r.members == std::vector<std::uint32_t>{0, 1, 2, 3, 4})
        whole = true;
    require(whole, "cross-block diamond was not structurally SESE");
    auto disconnected = diamond;
    disconnected.successors.push_back({7});
    disconnected.successors.push_back({});
    const auto reachable = structural_regions(disconnected);
    for (const auto &r : reachable.regions)
      for (const auto member : r.members)
        require(member < 6, "unreachable component became a scoped region");
    RegionGraph cycle{{{1}, {2}, {1, 3}, {}}, 0, 3};
    const auto cyclic = structural_regions(cycle);
    for (const auto &r : cyclic.regions)
      require(r.members != std::vector<std::uint32_t>{0, 1, 2},
              "finite-exit postdominance fabricated termination of a cycle");
    auto module = fixture();
    const auto report = StepRegionFormation::analyze(module, 0);
    require(report.ssa_valid, "generic diamond fixture must contain valid SSA");
    require(!report.budget_exhausted, "small generic region exhausted budget");
    bool joined = false, fatal = false, output = false;
    for (const auto &region : report.regions) {
      bool left = false, right = false, join = false;
      for (const auto at : region.instructions) {
        left |= at.block == 1;
        right |= at.block == 2;
        join |= at.block == 3 && at.index == 0;
        fatal |= at.block == 1 && at.index == 3;
      }
      if (left && right && join)
        joined = true;
      output |= std::find(region.outputs.begin(), region.outputs.end(), 7) !=
                region.outputs.end();
    }
    require(joined && fatal && output,
            "maximal typed region lost joined blocks, original fatal scalar or "
            "SSA output");
    bool io = false, data = false;
    for (const auto &gap : report.exclusions) {
      io |= gap.reason == RegionReason::ExternalIo;
      data |= gap.reason == RegionReason::DeviceDataRead;
    }
    require(io && data, "real I/O/device-read boundaries were hidden");
    auto invalid = module;
    invalid.functions[0].blocks[3].instructions[1] = Print{};
    const auto rejected = StepRegionFormation::analyze(invalid, 0);
    require(!rejected.ssa_valid && rejected.regions.empty() &&
                std::any_of(rejected.exclusions.begin(), rejected.exclusions.end(),
                            [](const auto &gap) {
                              return gap.reason == RegionReason::InvalidSsa;
                            }),
            "zero-valued mandatory Print operands bypassed SSA rejection");
    auto loop = fixture();
    auto &fn = loop.functions[0];
    fn.blocks[3].instructions.back() = Jump{"head"};
    fn.blocks.insert(
        fn.blocks.begin(),
        Block{"head", {ConstantBool{100, true}, Branch{100, "entry", "done"}}});
    fn.blocks.push_back({"done", {ReturnVoid{}}});
    const auto iteration = StepRegionFormation::analyze(loop, 0);
    bool loop_region = false;
    for (const auto &region : iteration.regions)
      loop_region |= region.scope == RegionScope::LoopIteration;
    require(loop_region, "one-iteration cross-block regions were replaced by "
                         "body components only");
    Module written;
    Function state;
    state.name = "generic_loop_state";
    const auto integer = Type::simple(TypeKind::Int64);
    const auto print_result = Type::union_of(
        {Type::simple(TypeKind::Void), Type::simple(TypeKind::Error)});
    state.blocks = {
        {"entry",
         {DeclareLocal{"state", integer, ""}, ConstantInt{1, "0", integer},
          StoreLocal{"state", 1, integer}, Jump{"head"}}},
        {"head",
         {LoadLocal{2, "state", integer}, ConstantBool{3, true},
          Branch{3, "body", "done"}}},
        {"body",
         {ConstantInt{4, "1", integer}, StoreLocal{"state", 4, integer},
          Print{4, integer, 6, print_result}, ConstantInt{5, "2", integer},
          StoreLocal{"state", 5, integer}, Jump{"head"}}},
        {"done", {ReturnVoid{}}}};
    written.functions = {state};
    const auto windows = StepRegionFormation::analyze(written, 0);
    require(windows.ssa_valid, "writer-window fixture must contain valid SSA");
    bool unavailable = false;
    for (const auto &gap : windows.exclusions)
      unavailable |= gap.reason == RegionReason::ExclusiveWindowUnavailable;
    require(unavailable, "outside writer produced no logical window gap");
    for (const auto &region : windows.regions)
      if (region.scope == RegionScope::LoopIteration)
        for (const auto at : region.instructions)
          require(!(at.block == 2 && (at.index == 1 || at.index == 4)),
                  "separated loop writers were both treated as exclusive");
    const auto limited = StepRegionFormation::analyze(module, 0, {0});
    require(limited.budget_exhausted && limited.regions.empty(),
            "bounded fallback retained uncertified partial candidates");
    std::puts("private step-region unit passed");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
#else
#include <cstdio>
int main() {
  std::fputs("missing structural step-region formation\n", stderr);
  return 1;
}
#endif
