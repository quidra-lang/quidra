#include "optimizer/finite_check_merging.hpp"
#include "optimizer/ownership_adaptation.hpp"
#include "optimizer/textual_uses.hpp"
#include "quidra/ir/value_allocator.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::optimizer;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void typed_uses_do_not_depend_on_display() {
    require(instruction_mentions_value(TimeSeconds{2, 41}, 41), "unrendered operands are uses");
    require(!instruction_mentions_value(ConstantInt{41, "7", {}}, 41),
            "a definition alone is not a use");
    require(instruction_mentions_value(ArrayMake{2, {41}, {}}, 41),
            "compact array text need not show every element");
    require(!instruction_mentions_value(ConstantString{2, "%41"}, 41),
            "a literal spelling is not an operand");
    Function function;
    function.blocks.push_back({"entry", {ConstantInt{1, "7", {}}, TimeSeconds{2, 1}, Release{1, {}}}});
    require(has_later_or_cross_block_use(function, &function.blocks[0].instructions[0], 1),
            "an unrendered later use keeps a value live");
    function.blocks[0].instructions.erase(function.blocks[0].instructions.begin() + 1);
    require(!has_later_or_cross_block_use(function, &function.blocks[0].instructions[0], 1),
            "release alone does not keep a value live");
}

void fresh_floor_sees_all_outputs_and_ignores_metadata() {
    Function function;
    function.blocks.push_back({"entry", {ParseNumberDirect{2, 71, 72, 1, {}, {0, 0}},
                                        ConstantString{3, "%999"}}});
    seed_value_counter(function);
    require(next_rewrite_value(function) == 73, "all outputs contribute to the fresh ID floor");
    function.blocks[0].instructions.push_back(TimeSeconds{4, 81});
    seed_value_counter(function);
    require(next_rewrite_value(function) == 82, "uses also protect against malformed-input ID collisions");
    function.blocks[0].instructions.push_back(ConstantBool{std::numeric_limits<ValueId>::max(), false});
    seed_value_counter(function);
    require(!next_rewrite_value(function), "the largest ValueId is exhausted without wraparound");
}

void finite_checks_keep_a_value_observed_elsewhere() {
    const auto real = Type::simple(TypeKind::Real64);
    Module module;
    Function function;
    function.blocks.push_back({"entry", {
        Binary{3, "+", 1, 2, real, real},
        FiniteCheck{{CheckedReal{3, real, {1, 1}, FiniteCause::addition, 1, 2}}, {}},
        Binary{4, "*", 3, 2, real, real},
        FiniteCheck{{CheckedReal{4, real, {1, 1}, FiniteCause::multiplication, 3, 2}}, {}},
        TimeSeconds{5, 3}, ReturnVoid{}}});
    module.functions.push_back(std::move(function));
    seed_value_counter(module.functions[0]);
    const auto registry = RuleRegistry::build(module);
    const FunctionIndex functions(module);
    ir::OptimizerOptions options;
    PassContext context{registry, functions, options};
    require(!FiniteCheckMerging::run(module, context),
            "a separately observed result prevents finite-check merging");
    auto& instructions = module.functions[0].blocks[0].instructions;
    instructions.erase(instructions.begin() + 4);
    require(FiniteCheckMerging::run(module, context), "a single propagating consumer permits merging");
    const auto& merged = std::get<FiniteCheck>(instructions[2]);
    require(merged.primary[0].value == 4 && merged.covered.size() == 1 && merged.covered[0].value == 3,
            "merging preserves the covered failure order");
}

Module finite_loop(std::uint64_t next) {
    const auto real = Type::simple(TypeKind::Real64);
    Module module;
    Function function;
    function.next_value = next;
    function.blocks = {
        {"entry", {ConstantBool{1, true}, ConstantFloat{2, 1.0, real, {}},
                   ConstantFloat{3, 2.0, real, {}}, Jump{"for.range.cond.0"}}},
        {"for.range.cond.0", {Branch{1, "for.range.body.1", "for.range.end.3"}}},
        {"for.range.body.1", {Binary{4, "+", 2, 3, real, real},
                              FiniteCheck{{CheckedReal{4, real, {1, 1}, FiniteCause::addition, 2, 3}}, {}},
                              Jump{"for.range.advance.2"}}},
        {"for.range.advance.2", {Jump{"for.range.cond.0"}}},
        {"for.range.end.3", {ReturnVoid{}}}};
    module.functions.push_back(std::move(function));
    return module;
}

void rewrites_share_the_persistent_cursor() {
    auto module = finite_loop(200);
    const auto registry = RuleRegistry::build(module);
    const FunctionIndex functions(module);
    ir::OptimizerOptions options;
    const PassContext context{registry, functions, options};
    require(FiniteCheckMerging::run(module, context), "the quiet loop permits check sinking");
    const auto& entry = module.functions[0].blocks[0].instructions;
    require(std::get<ConstantBool>(entry[3]).out == 200 && module.functions[0].next_value == 201,
            "finite sinking allocates after reserved IDs, not current displayed values");
    auto exhausted = finite_loop(std::uint64_t{std::numeric_limits<ValueId>::max()} + 1);
    const auto exhausted_registry = RuleRegistry::build(exhausted);
    const FunctionIndex exhausted_functions(exhausted);
    const PassContext exhausted_context{exhausted_registry, exhausted_functions, options};
    require(!FiniteCheckMerging::run(exhausted, exhausted_context), "exhausted sinking declines before changing the loop");
    require(std::holds_alternative<FiniteCheck>(exhausted.functions[0].blocks[2].instructions[1]),
            "a declined loop retains its original finite check");

    Function replacement;
    replacement.name = "replacement";
    Function caller;
    caller.next_value = 300;
    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32));
    replacement.parameters.push_back({"value", tensor});
    replacement.result = tensor;
    caller.blocks.push_back({"entry", {Call{10, "original", {{1, {}}}, tensor}}});
    const OwnershipAdaptation adaptation{&replacement, {}, {}, {{0, tensor}}};
    auto& block = caller.blocks[0];
    require(adaptation.apply(caller, block, block.instructions[0]), "ownership adaptation permits its clone");
    require(std::get<Clone>(block.instructions[0]).out == 300 &&
                std::get<Call>(block.instructions[1]).args[0].value == 300 && caller.next_value == 301,
            "ownership clone consumes the shared persistent allocation cursor");
    const auto before = block.instructions.size();
    caller.next_value = std::uint64_t{std::numeric_limits<ValueId>::max()} + 1;
    require(!adaptation.apply(caller, block, block.instructions[1]) && block.instructions.size() == before,
            "exhausted ownership adaptation declines without inserting instructions");
}
} // namespace

int main() {
    try {
        typed_uses_do_not_depend_on_display();
        fresh_floor_sees_all_outputs_and_ignores_metadata();
        finite_checks_keep_a_value_observed_elsewhere();
        rewrites_share_the_persistent_cursor();
        std::cout << "optimizer operands: typed uses, output floor and finite checks verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
