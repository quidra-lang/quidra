#pragma once
#include "ir/analysis/cfg.hpp"
#include "quidra/ir/visit.hpp"
#include <cstdint>
#include <vector>
namespace quidra::ir { struct Module; }
namespace quidra::ir::analysis {
struct Use {
    InstrRef at;
    OperandRole role{OperandRole::Read};
    bool role_available{true};
    std::uint32_t ordinal{};
    bool operator==(const Use&) const = default;
};
struct ValueRecord {
    ValueId value{};
    std::vector<InstrRef> definitions;
    std::vector<Use> uses;
};
// Sorted, owned records for actual IDs only. Definitions and repeated uses
// retain authored occurrence order, including malformed zero IDs.
struct DefUse {
    std::vector<ValueRecord> values;
    ValueId max_value{};
    const ValueRecord* find(ValueId) const noexcept;
};
DefUse def_use(const Function&, const Module* module = nullptr);
} // namespace quidra::ir::analysis
