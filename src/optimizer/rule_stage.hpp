#pragma once

// RuleStage: the stage of a conditional rule, given by its table
// (`[specialization.<id>]`, `[backend.<id>]`, `[memory.<id>]`). Rules are
// tried in stage order, and the stage decides how a replacement may differ
// from the original in ownership: only a memory-reuse replacement may take
// over the release of an owned temporary, and only the other stages may
// give a by-value replacement a clone.

namespace quidra::optimizer {

enum class RuleStage : int { Specialization = 0, BackendSelection = 1, MemoryReuse = 2 };

} // namespace quidra::optimizer
