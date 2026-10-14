#pragma once

// Typed-IR instructions of the concurrency domain (parallel blocks).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/call.hpp"
#include <string>
#include <optional>
#include <cstddef>
#include <vector>
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>

namespace quidra::ir {

struct ParallelNode {
    std::string function;
    std::vector<CallArgument> captures;
    std::vector<std::uint32_t> predecessors;
    SourcePosition position{};
    bool fallible{};
    bool worker_safe{};
};
struct ParallelArrayReceipt {
    std::size_t capture{}; ValueId array{}; bool read_only{}; bool bare_integer{};
};
struct ParallelIteration {
    ValueId count{};
    std::optional<ValueId> worker_safe{};
    std::vector<ParallelArrayReceipt> arrays{};
};
struct ParallelBlock {
    ValueId out{}; // Nullable owned void|error; null means every node succeeded.
    std::string table_name;
    std::vector<ParallelNode> nodes;
    SourcePosition position{};
    std::optional<ParallelIteration> iterations{};
};
template <> struct InstructionTraits<ParallelBlock> : WithOperands<Domain::concurrency, ParallelBlock> {};



} // namespace quidra::ir
