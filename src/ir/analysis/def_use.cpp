#include "ir/analysis/def_use.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace quidra::ir::analysis {
const ValueRecord* DefUse::find(ValueId value) const noexcept {
    const auto found = std::lower_bound(values.begin(), values.end(), value,
        [](const ValueRecord& record, ValueId id) { return record.value < id; });
    return found != values.end() && found->value == value ? &*found : nullptr;
}

DefUse def_use(const Function& function, const Module* module) {
    if (function.blocks.size() > std::numeric_limits<BlockId>::max())
        throw std::length_error("def-use block index overflow");
    std::map<ValueId, ValueRecord> records;
    struct Collector final : OperandVisitor {
        std::map<ValueId, ValueRecord>& records;
        InstrRef at{};
        std::uint64_t ordinal{};
        explicit Collector(std::map<ValueId, ValueRecord>& storage) : records(storage) {}
        ValueRecord& record(ValueId id) {
            return records.try_emplace(id, ValueRecord{id, {}, {}}).first->second;
        }
        void definition(ValueId id) override { record(id).definitions.push_back(at); }
        void use(ValueId id, OperandUse operand) override {
            if (ordinal > std::numeric_limits<std::uint32_t>::max())
                throw std::length_error("def-use operand ordinal overflow");
            record(id).uses.push_back({at, operand.role, operand.role_available,
                                      static_cast<std::uint32_t>(ordinal++)});
        }
    } collector(records);
    const OperandContext context{module};
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
        const auto& instructions = function.blocks[block].instructions;
        if (instructions.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("def-use instruction index overflow");
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            collector.at = {static_cast<BlockId>(block), static_cast<std::uint32_t>(index)};
            collector.ordinal = 0;
            visit_operands(instructions[index], collector, context);
        }
    }
    DefUse result;
    result.values.reserve(records.size());
    for (auto& [value, record] : records) {
        result.max_value = value;
        result.values.push_back(std::move(record));
    }
    return result;
}
} // namespace quidra::ir::analysis
