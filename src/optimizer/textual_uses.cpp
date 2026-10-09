// Textual value uses (textual_uses.hpp).
#include "optimizer/textual_uses.hpp"

#include "ir/instruction_text.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace quidra::optimizer {

using ir::Block;
using ir::Function;
using ir::Instruction;
using ir::Release;
using ir::ValueId;
using ir::instruction_text;

bool instruction_mentions_value(const Instruction& instruction, ValueId value) {
    const auto rendered = instruction_text(instruction);
    const auto token = "%" + std::to_string(value);
    std::size_t position = 0;
    while ((position = rendered.find(token, position)) !=
           std::string::npos) {
        const auto end = position + token.size();
        if (end == rendered.size() ||
            rendered[end] < '0' || rendered[end] > '9') {
            return true;
        }
        position = end;
    }
    return false;
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
    std::uint64_t maximum = 0;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            const auto rendered = instruction_text(instruction);
            std::size_t position = 0;
            while ((position = rendered.find('%', position)) !=
                   std::string::npos) {
                std::size_t cursor = position + 1;
                if (cursor >= rendered.size() ||
                    rendered[cursor] < '0' ||
                    rendered[cursor] > '9') {
                    position = cursor;
                    continue;
                }
                std::uint64_t parsed = 0;
                while (cursor < rendered.size() &&
                       rendered[cursor] >= '0' &&
                       rendered[cursor] <= '9') {
                    parsed =
                        parsed * 10 +
                        static_cast<std::uint64_t>(
                            rendered[cursor] - '0');
                    ++cursor;
                }
                maximum = std::max(maximum, parsed);
                position = cursor;
            }
        }
    }
    if (maximum >=
        std::numeric_limits<ValueId>::max()) {
        return std::nullopt;
    }
    return static_cast<ValueId>(maximum + 1);
}

} // namespace quidra::optimizer
