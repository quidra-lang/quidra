#pragma once

// The axis specs that generated code passes to quidra_tensor_index: for
// each indexed axis, `fields` i64 values, the kind of the axis's item, then
// its parts. An index item has one part, the index; a slice has three, its
// start, stop and step. Generated slices use marked forms, which name
// whether each bound was written, and the explicit_step bit names whether
// the step was written. Thus a written value, including INT64_MIN, cannot
// be confused with omission. Unflagged marked slices retain the missing
// step convention. The legacy `slice` kind retains its half-open range
// start:<end and missing-part convention for existing runtime callers.

#include "quidra/slice_form.hpp"

#include <limits>

namespace quidra::abi::tensor_index_spec {

inline constexpr int fields = 4;
// The field of the item's kind (index or slice), and of its first part; the
// other parts follow it.
inline constexpr int kind_field = 0;
inline constexpr int first_part_field = 1;
inline constexpr long long index = 0;
inline constexpr long long slice = 1;
// A tensor<bool> selector is lowered as a single mask selection, not as
// an integer index. The LLVM emitter dispatches it to the mask runtime.
inline constexpr long long mask = 64;
inline constexpr long long missing = std::numeric_limits<long long>::min();
inline constexpr long long marked_slice = 2;
inline constexpr long long explicit_step = 32;

// The kind of a marked slice, and its forms back.
constexpr long long marked_slice_kind(SliceStart start, SliceEnd end) {
    return marked_slice + 4 * static_cast<long long>(start) + static_cast<long long>(end);
}
constexpr bool is_marked_slice(long long kind) {
    const auto forms = kind & ~explicit_step;
    return forms >= marked_slice && forms < marked_slice + 16;
}
constexpr SliceStart marked_start(long long kind) {
    return static_cast<SliceStart>(((kind & ~explicit_step) - marked_slice) / 4);
}
constexpr SliceEnd marked_end(long long kind) {
    return static_cast<SliceEnd>(((kind & ~explicit_step) - marked_slice) % 4);
}

} // namespace quidra::abi::tensor_index_spec
