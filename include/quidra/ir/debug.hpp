#pragma once

// Typed-IR instructions of the debug domain (source locations for runtime provenance and debug info).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"

#include <cstdint>
#include <string>

namespace quidra::ir {

// Runtime/debug attribution only; optimizer decisions must not inspect it.
struct SourcePosition { std::uint32_t line{}; std::uint32_t column{}; };

struct SourceLocation {
    SourcePosition position{};
    std::string source_file;
    std::string source_revision;
    std::string node_id;
    std::string node_kind;
};

// Semantic boundary of one user statement, scoped to its function.
// Inlining must remap the ordinal per call site; debug attribution is separate.
struct StatementBoundary { std::uint64_t identity{}; };

template <> struct InstructionTraits<StatementBoundary> : WithOperands<Domain::debug, StatementBoundary> {};

template <> struct InstructionTraits<SourceLocation> : WithOperands<Domain::debug, SourceLocation> {};

} // namespace quidra::ir
