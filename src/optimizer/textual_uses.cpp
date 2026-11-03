// Typed value uses (textual_uses.hpp keeps the existing helper names).
#include "optimizer/textual_uses.hpp"

#include "quidra/ir/visit.hpp"

#include <limits>

namespace quidra::optimizer {

using ir::Block;
using ir::Function;
using ir::Instruction;
using ir::Release;
using ir::ValueId;

bool instruction_mentions_value(const Instruction& instruction, ValueId value) {
    struct FindUse final : ir::OperandVisitor {
        explicit FindUse(ValueId sought) : sought(sought) {}
        void use(ValueId value) override { found = found || value == sought; }
        ValueId sought;
        bool found{};
    } visitor(value);
    ir::visit_operands(instruction, visitor);
    return visitor.found;
}

bool has_later_or_cross_block_use(const Function& function, const Instruction* current,
                                  ValueId value) {
    const Block* current_block = nullptr;
    std::size_t current_index = 0;
    for (const auto& block : function.blocks) {
        for (std::size_t index = 0;
             index < block.instructions.size(); ++index) {
            if (&block.instructions[index] == current) {
                current_block = &block;
                current_index = index;
                break;
            }
        }
        if (current_block) break;
    }
    if (!current_block) return true;

    for (std::size_t index = current_index + 1;
         index < current_block->instructions.size(); ++index) {
        const auto& instruction =
            current_block->instructions[index];
        if (const auto* release =
                std::get_if<Release>(&instruction);
            release && release->value == value) {
            continue;
        }
        if (instruction_mentions_value(instruction, value))
            return true;
    }
    // Without a CFG liveness proof, any cross-block reference keeps the
    // value live. This is deliberately conservative for reuse rules.
    for (const auto& block : function.blocks) {
        if (&block == current_block) continue;
        for (const auto& instruction : block.instructions) {
            if (const auto* release =
                    std::get_if<Release>(&instruction);
                release && release->value == value) {
                continue;
            }
            if (instruction_mentions_value(instruction, value))
                return true;
        }
    }
    return false;
}

std::optional<ValueId> next_rewrite_value(const Function& function) {
    if (function.next_value == 0 || function.next_value > std::numeric_limits<ValueId>::max())
        return std::nullopt;
    return static_cast<ValueId>(function.next_value);
}

} // namespace quidra::optimizer
