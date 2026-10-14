#include "quidra/ir/value_allocator.hpp"
#include "quidra/ir/visit.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace quidra::ir {

void seed_value_counter(Function& function) {
    struct Floor final : OperandVisitor {
        void definition(ValueId value) override { observe(value); }
        void use(ValueId value) override { observe(value); }
        void observe(ValueId value) { next = std::max(next, std::uint64_t{value} + 1); }
        std::uint64_t next{1};
    } visitor;
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            visit_operands(instruction, visitor);
    function.next_value = std::max(function.next_value, visitor.next);
}

std::optional<ValueId> allocate_value(Function& function) {
    if (function.next_value == 0 || function.next_value > std::numeric_limits<ValueId>::max())
        return std::nullopt;
    const auto value = static_cast<ValueId>(function.next_value);
    ++function.next_value;
    return value;
}

} // namespace quidra::ir
