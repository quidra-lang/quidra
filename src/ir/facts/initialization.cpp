#include "ir/facts/initialization.hpp"

namespace quidra::ir::facts {
FullArrays intersect_full_arrays(const FullArrays& left, const FullArrays& right) {
    FullArrays result;
    const auto& small = left.size() <= right.size() ? left : right;
    const auto& large = left.size() <= right.size() ? right : left;
    for (const auto& name : small) if (large.contains(name)) result.insert(name);
    return result;
}
} // namespace quidra::ir::facts
