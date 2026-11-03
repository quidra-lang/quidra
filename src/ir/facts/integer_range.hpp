#pragma once

#include <cstdint>

namespace quidra::ir::facts {

// Inclusive bounds of a nonnegative fixed-width or bare integer value.
// The representation and arithmetic interpretation are shared by lowering
// and optimizer clients; acquiring a source/CFG proof belongs to the client.
struct NonnegativeIntegerRange {
    std::uint64_t minimum{};
    std::uint64_t maximum{};
};

} // namespace quidra::ir::facts
