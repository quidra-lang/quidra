#pragma once
#include "ir/canonical_encoding.hpp"

#include <array>
#include <cstdint>

namespace quidra::ir {
struct Digest {
    std::array<std::uint8_t,32> bytes{};
    bool operator==(const Digest&) const = default;
};
struct FunctionIdentity {
    Digest structural;
    Digest locations;
    bool operator==(const FunctionIdentity&) const = default;
};
FunctionIdentity identify(const Function&);
// Requires the same source-slot schema/order. Incomplete, surplus or
// inconsistent tables throw invalid_argument before returning a function.
Function stitch(Function structural, const LocationTable&);
} // namespace quidra::ir
