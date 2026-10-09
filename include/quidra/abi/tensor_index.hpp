#pragma once

// The axis specs that generated code passes to quidra_tensor_index: for
// each indexed axis, `fields` i64 values, the kind of the axis's item, then
// its parts. An index item has one part, the index; a slice has three, its
// start, stop and step. A part that is not given is `missing`.

#include <limits>

namespace quidra::abi::tensor_index_spec {

inline constexpr int fields = 4;
// The field of the item's kind (index or slice), and of its first part; the
// other parts follow it.
inline constexpr int kind_field = 0;
inline constexpr int first_part_field = 1;
inline constexpr long long index = 0;
inline constexpr long long slice = 1;
inline constexpr long long missing = std::numeric_limits<long long>::min();

} // namespace quidra::abi::tensor_index_spec
