#pragma once

// Persistent per-function value allocation. Lowering and rewrites allocate
// from the same cursor; deleting an instruction never makes its ID reusable.
// Imported or hand-built IR must be seeded once before rewriting. Seeding
// visits typed definitions and uses, never diagnostic text or literal names.

#include "quidra/ir/function.hpp"

#include <optional>

namespace quidra::ir {

void seed_value_counter(Function& function);

// Returns no value on exhaustion and leaves the cursor unchanged. The final
// representable ValueId is available; the uint64 cursor then records max+1.
std::optional<ValueId> allocate_value(Function& function);

} // namespace quidra::ir
