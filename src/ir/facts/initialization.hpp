#pragma once

#include <string>
#include <unordered_set>

namespace quidra::ir::facts {

using FullArrays = std::unordered_set<std::string>;
FullArrays intersect_full_arrays(const FullArrays& left, const FullArrays& right);

} // namespace quidra::ir::facts
