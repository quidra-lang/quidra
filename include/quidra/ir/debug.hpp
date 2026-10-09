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

struct SourceLocation {
    std::uint32_t line{};
    std::uint32_t column{};
    std::string source_file;
    std::string source_revision;
    std::string node_id;
    std::string node_kind;
};

template <> struct InstructionTraits<SourceLocation> : InDomain<Domain::debug> {};

} // namespace quidra::ir
