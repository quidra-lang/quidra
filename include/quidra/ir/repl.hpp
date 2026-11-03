#pragma once

// Typed-IR instructions of the repl domain (REPL display and replay).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <string>
#include <vector>

namespace quidra::ir {

struct ReplDisplay {
    ValueId value;
    Type type;
    std::vector<std::string> initialized_paths;
};
struct ReplReplayMode { bool active{}; };

template <> struct InstructionTraits<ReplDisplay> : WithOperands<Domain::repl, ReplDisplay> {};
template <> struct InstructionTraits<ReplReplayMode> : WithOperands<Domain::repl, ReplReplayMode> {};

} // namespace quidra::ir
