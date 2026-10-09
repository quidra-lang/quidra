#pragma once

// ValueId: the number of an IR value (%N). ir::FunctionBuilder allocates them
// per function. Every domain header includes this header.

#include <cstdint>

namespace quidra::ir {

using ValueId = std::uint32_t;

} // namespace quidra::ir
