#include "ir/function_builder.hpp"
#include "quidra/ir/value_allocator.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void persistent_function_counter() {
    Function first, second;
    FunctionBuilder a(first, 1, 0, 0), b(second, 1, 0, 0);
    require(a.fresh() == 1 && a.fresh() == 2 && b.fresh() == 1,
            "each function has its own ID sequence");
    a.finish();
    b.finish();
    require(first.next_value == 3 && second.next_value == 2, "finish retains each allocation cursor");
    Function copied = first;
    FunctionBuilder resumed(copied, 1, 0, 0);
    require(resumed.fresh() == 3, "resuming a copied function does not reuse removed IDs");
    resumed.finish();
    require(copied.next_value == 4 && first.next_value == 3, "copies own independent allocation state");
    first.next_value = 101;
    FunctionBuilder after_removed_definition(first, 1, 0, 0);
    require(after_removed_definition.fresh() == 101, "rewrites allocate from the cursor, not rendered IDs");
    after_removed_definition.finish();
}

void imported_ir_seeding_is_explicit_and_typed() {
    Function function;
    function.blocks.push_back({"entry", {ParseNumberDirect{2, 71, 72, 1, {}, {0, 0}},
                                        ConstantString{3, "%999"}, TimeSeconds{4, 81}}});
    seed_value_counter(function);
    require(function.next_value == 82, "seeding sees multioutput definitions and unrendered uses");
    require(allocate_value(function) == 82 && function.next_value == 83, "allocation advances persistent state");
    function.blocks.clear();
    seed_value_counter(function);
    require(function.next_value == 83, "seeding never lowers a cursor after dead values are removed");
}

void exhaustion_never_wraps() {
    Function function;
    function.next_value = std::numeric_limits<ValueId>::max();
    FunctionBuilder builder(function, 1, 0, 0);
    require(builder.fresh() == std::numeric_limits<ValueId>::max(), "the final representable ID may be allocated");
    builder.finish();
    require(function.next_value == std::uint64_t{std::numeric_limits<ValueId>::max()} + 1,
            "the cursor represents exhaustion without wrapping to zero");
    require(!allocate_value(function), "rewriters may decline on exhaustion");
    bool threw = false;
    try { (void)builder.fresh(); } catch (const std::overflow_error&) { threw = true; }
    require(threw, "mandatory lowering allocation reports exhaustion");
    Function imported;
    imported.blocks.push_back({"entry", {ConstantBool{std::numeric_limits<ValueId>::max(), false}}});
    seed_value_counter(imported);
    require(!allocate_value(imported), "a typed maximum ID cannot collide with a fresh value");
}
} // namespace

int main() {
    try {
        persistent_function_counter();
        imported_ir_seeding_is_explicit_and_typed();
        exhaustion_never_wraps();
        std::cout << "IR values: persistent per-function cursor and exhaustion verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
