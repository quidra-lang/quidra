#pragma once

// Shared slice index planner for arrays, strings, bins and tensor views.
// The parser supplies endpoint directions (< before : or > before :, and
// :< or :> after the colon), but the index semantics live here. All
// arithmetic is exact, integer-only, and independent of container storage.
#include <cstdint>
#include <limits>
#include <optional>

namespace quidra {

enum class SliceDirection : std::uint8_t { automatic, ascending, descending };
enum class SliceStatus : std::uint8_t {
    ok, length_overflow, zero_step, direction_conflict,
    endpoint_out_of_bounds, empty_inclusive_range
};

struct SliceRequest {
    std::optional<std::int64_t> start;
    std::optional<std::int64_t> end;
    std::optional<std::int64_t> step;
    bool exclude_start = false;
    bool exclude_end = false;
    SliceDirection start_marker = SliceDirection::automatic;
    SliceDirection end_marker = SliceDirection::automatic;
};

struct SliceIndexPlan {
    std::int64_t first = 0;
    std::int64_t step = 1;
    std::uint64_t count = 0;

    // Call only for offset < count. The planner guarantees that the
    // multiplication cannot overflow a 64-bit signed position.
    constexpr std::int64_t index_at(std::uint64_t offset) const {
        const auto magnitude = step < 0
            ? std::uint64_t(-(step + 1)) + 1u
            : static_cast<std::uint64_t>(step);
        const auto distance = offset * magnitude;
        const auto position = static_cast<std::uint64_t>(first);
        return static_cast<std::int64_t>(
            step < 0 ? position - distance : position + distance);
    }
};

struct SlicePlanResult {
    SliceIndexPlan plan{};
    SliceStatus status = SliceStatus::ok;
    constexpr explicit operator bool() const { return status == SliceStatus::ok; }
};

constexpr SlicePlanResult plan_slice(std::uint64_t length, const SliceRequest& request) {
    constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (length > max) return {{}, SliceStatus::length_overflow};
    const auto n = static_cast<std::int64_t>(length);

    if (request.start_marker != SliceDirection::automatic &&
        request.end_marker != SliceDirection::automatic &&
        request.start_marker != request.end_marker)
        return {{}, SliceStatus::direction_conflict};

    SliceDirection direction = request.start_marker != SliceDirection::automatic
        ? request.start_marker : request.end_marker;
    const std::int64_t step =
        request.step.value_or(direction == SliceDirection::descending ? -1 : 1);
    if (step == 0) return {{}, SliceStatus::zero_step};
    const auto sign = step < 0 ? SliceDirection::descending : SliceDirection::ascending;
    if (direction != SliceDirection::automatic && direction != sign)
        return {{}, SliceStatus::direction_conflict};
    direction = sign;

    // Defaults depend on direction, but explicit endpoints are validated
    // even when the resulting selection is empty.
    const auto start = request.start.value_or(
        direction == SliceDirection::ascending ? 0 : n - 1);
    const auto end = request.end.value_or(
        direction == SliceDirection::ascending ? n - 1 : 0);

    const auto valid = [n](std::int64_t point, bool excluded) constexpr {
        if (excluded) return point >= -1 && point <= n;
        return point >= 0 && point < n;
    };
    // A full slice of an empty collection is empty. An explicit ascending
    // start at length, with no end, is also the canonical empty suffix
    // (a[len(a):]); unlike an explicit inclusive end, this is not out of
    // bounds. Every other explicit endpoint is still checked strictly.
    if (n == 0 && !request.start && !request.end)
        return {{0, step, 0}, SliceStatus::ok};
    if (direction == SliceDirection::ascending && request.start &&
        start == n && (!request.end ||
                       (request.exclude_end && request.end && end == n)))
        return {{0, step, 0}, SliceStatus::ok};
    if ((request.start && !valid(start, request.exclude_start)) ||
        (request.end && !valid(end, request.exclude_end)))
        return {{}, SliceStatus::endpoint_out_of_bounds};
    if ((!request.start && !valid(start, request.exclude_start)) ||
        (!request.end && !valid(end, request.exclude_end)))
        return {{0, step, 0}, SliceStatus::ok};

    const std::uint64_t stride = step < 0
        ? std::uint64_t(-(step + 1)) + 1u
        : static_cast<std::uint64_t>(step);
    const bool may_be_empty = request.exclude_start || request.exclude_end;

    // Compute first and the inclusive terminal bound without overflowing:
    // excluded start steps once; excluded end tightens once.
    if (direction == SliceDirection::ascending) {
        const std::uint64_t first = start < 0
            ? stride - 1u
            : static_cast<std::uint64_t>(start) + stride * (request.exclude_start ? 1u : 0u);
        const auto last = end - (request.exclude_end ? 1 : 0);
        if (last < 0 || first > static_cast<std::uint64_t>(last))
            return may_be_empty ? SlicePlanResult{{0, step, 0}, SliceStatus::ok}
                                : SlicePlanResult{{}, SliceStatus::empty_inclusive_range};
        if (first >= length)
            return {{}, SliceStatus::endpoint_out_of_bounds};
        const auto gap = static_cast<std::uint64_t>(last) - first;
        return {{static_cast<std::int64_t>(first), step, 1u + gap / stride}, SliceStatus::ok};
    }

    // Negative step including INT64_MIN: subtraction uses unsigned
    // magnitude and never negates INT64_MIN.
    // The excluded upper sentinel may be INT64_MAX; its successor is
    // INT64_MAX+1, which must never be formed in signed arithmetic.
    // An excluded -1 maps to the first valid index (zero).
    const std::uint64_t last = end < 0
        ? 0u
        : static_cast<std::uint64_t>(end) + (request.exclude_end ? 1u : 0u);
    const std::uint64_t anchor = start < 0 ? 0 : static_cast<std::uint64_t>(start);
    if (request.exclude_start && (start < 0 || anchor < stride))
        return {{0, step, 0}, SliceStatus::ok};
    const std::uint64_t first = request.exclude_start ? anchor - stride : anchor;
    if (first < last)
        return may_be_empty ? SlicePlanResult{{0, step, 0}, SliceStatus::ok}
                            : SlicePlanResult{{}, SliceStatus::empty_inclusive_range};
    if (first >= length)
        return {{}, SliceStatus::endpoint_out_of_bounds};
    return {{static_cast<std::int64_t>(first), step,
             1u + (first - last) / stride}, SliceStatus::ok};
}

} // namespace quidra
