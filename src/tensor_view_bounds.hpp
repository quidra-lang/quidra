#pragma once
#include <cstddef>
#include <limits>
#include <vector>

// Tensor view addressing shared by the runtime and its unit tests.
//
// A tensor view is (offset, shape, strides) over a storage of storage_count
// elements. storage_index() is the runtime's definition of where a logical
// row-major element lives; it deliberately uses wrapping std::size_t
// arithmetic, so a view the runtime could not prove safe is still addressed
// exactly as before and rejected by the caller's per-element range check.
//
// within_storage() is the O(rank) proof that lets initialization checks and
// copies skip their per-element loops. It must never accept a view for which
// any storage_index() result reaches outside [0, storage_count).
namespace quidra::tensor_view {

inline std::size_t storage_index(std::size_t offset,
                                 const std::vector<long long>& shape,
                                 const std::vector<long long>& strides,
                                 std::size_t logical) {
    if (shape.empty()) return offset;
    std::size_t index = offset;
    for (std::size_t axis = shape.size(); axis-- > 0;) {
        const auto dimension = static_cast<std::size_t>(shape[axis]);
        const auto coordinate = dimension == 0 ? 0 : logical % dimension;
        if (dimension != 0) logical /= dimension;
        index += coordinate * static_cast<std::size_t>(strides[axis]);
    }
    return index;
}

// True only when every storage_index() of the view lies in
// [0, storage_count). Only non-negative extents and strides are accepted and
// every step is overflow-checked, so no per-element index sum can wrap and
// each one is bounded by offset + sum((extent - 1) * stride). A view that
// references no element is trivially inside storage. Anything unproven
// (negative strides, arithmetic overflow, a view that really reaches past the
// storage end) returns false and callers keep their exact per-element path.
inline bool within_storage(std::size_t offset,
                           const std::vector<long long>& shape,
                           const std::vector<long long>& strides,
                           std::size_t storage_count) {
    if (strides.size() != shape.size()) return false;
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    std::size_t last = offset;
    bool empty = false;
    for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        const auto extent = shape[axis];
        const auto stride = strides[axis];
        if (extent < 0 || stride < 0) return false;
        if (static_cast<unsigned long long>(extent) > size_limit ||
            static_cast<unsigned long long>(stride) > size_limit) {
            return false;
        }
        if (extent == 0) {
            empty = true;
            continue;
        }
        const auto span = static_cast<std::size_t>(extent - 1);
        const auto step = static_cast<std::size_t>(stride);
        if (step != 0 && span > size_limit / step) return false;
        const auto delta = span * step;
        if (delta > size_limit - last) return false;
        last += delta;
    }
    if (empty) return true;
    return last < storage_count;
}

// True when storage_index(offset, shape, strides, i) == offset + i for every
// logical element i, so the view is one dense row-major run of storage and can
// be copied with a single memcpy once offset + count fits in storage. Strides
// of extent-1 axes never contribute to an index and are ignored; a view with
// a zero extent has no elements and is trivially a run. Anything else must
// carry exactly the row-major strides, checked without overflow.
inline bool dense_run(const std::vector<long long>& shape,
                      const std::vector<long long>& strides) {
    if (strides.size() != shape.size()) return false;
    for (const auto extent : shape) {
        if (extent < 0) return false;
    }
    for (const auto extent : shape) {
        if (extent == 0) return true;
    }
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    std::size_t expected = 1;
    for (std::size_t axis = shape.size(); axis-- > 0;) {
        const auto extent = shape[axis];
        if (extent == 1) continue;
        if (static_cast<unsigned long long>(extent) > size_limit) return false;
        const auto stride = strides[axis];
        if (stride < 0 || static_cast<unsigned long long>(stride) != expected) return false;
        const auto size = static_cast<std::size_t>(extent);
        if (expected > size_limit / size) return false;
        expected *= size;
    }
    return true;
}

} // namespace quidra::tensor_view
