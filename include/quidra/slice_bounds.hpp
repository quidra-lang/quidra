#pragma once

// The slice plan: the index set a slice item selects, as
// (first, count, step), for arrays, strings (code points), bins and tensor
// axes alike. The rules live here once:
//   - direction: the markers (`<` ascending, `>` descending), else the
//     step's sign, else both explicit endpoints' order, else ascending;
//     two markers that disagree, a marker
//     against the step's sign, and a step of 0 are step errors;
//   - the default step is +1 ascending and -1 descending; an omitted start
//     is the inclusive 0 ascending or len - 1 descending, and an omitted end
//     runs through the last element (`:<len` ascending, `:>-1` descending);
//   - the start anchors the progression s, s + k, s + 2k, ...; an exclusive
//     start drops s itself; the progression stops at an inclusive end
//     (d*i <= d*e) or before an exclusive one (d*i < d*e);
//   - bounds, with no clamping: (1) every selected index is in [0, len);
//     (2) an exclusive endpoint is in [-1, len]; (3) an inclusive end is a
//     valid index, or len when the slice is empty; (4) so is an inclusive
//     start; (5) the start may pass the end in the slice's direction only
//     when a side is exclusive and no source-written step fixes the
//     opposite direction (the slice is then empty).
// All arithmetic is exact over 64-bit operands: no intermediate overflows.
#include "quidra/slice_form.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace quidra {

struct SliceRequest {
    SliceStart start_form = SliceStart::omitted;
    std::int64_t start = 0;
    SliceEnd end_form = SliceEnd::omitted;
    std::int64_t end = 0;
    std::optional<std::int64_t> step;
};

enum class SliceStatus : std::uint8_t {
    ok,
    length_overflow,
    // SLICE_STEP: two markers point different ways; the step is 0; the step
    // points against a marker.
    marker_conflict,
    zero_step,
    step_against_marker,
    // INDEX_BOUNDS: one of the bound rules 1-5.
    out_of_bounds,
};

struct SliceIndexPlan {
    std::int64_t first = 0;
    std::int64_t step = 1;
    std::uint64_t count = 0;

    // Call only for offset < count. Every selected index lies in [0, len),
    // so the position never leaves the signed range.
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

// Tensor adapters retain their separate positive-step contract, including
// reversed exclusive empty slices, by disabling only the explicit-step rule.
// Both-inclusive wrong-direction bound errors always remain enforced.
constexpr SlicePlanResult plan_slice(std::uint64_t length, const SliceRequest& request,
                                     bool strict_explicit_step_endpoints = true) {
    constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (length > max) return {{}, SliceStatus::length_overflow};
    const auto n = static_cast<std::int64_t>(length);

    // Direction and step.
    const int start_marker = slice_marker_direction(request.start_form);
    const int end_marker = slice_marker_direction(request.end_form);
    if (start_marker != 0 && end_marker != 0 && start_marker != end_marker)
        return {{}, SliceStatus::marker_conflict};
    int direction = start_marker != 0 ? start_marker : end_marker;
    std::int64_t step = 0;
    if (request.step) {
        step = *request.step;
        if (step == 0) return {{}, SliceStatus::zero_step};
        const int sign = step < 0 ? -1 : 1;
        if (direction != 0 && direction != sign) return {{}, SliceStatus::step_against_marker};
        direction = sign;
    } else {
        if (direction == 0)
            direction = request.start_form != SliceStart::omitted &&
                        request.end_form != SliceEnd::omitted && request.start > request.end
                      ? -1 : 1;
        step = direction;
    }
    const bool ascending = direction > 0;

    // The endpoints, omitted ones by direction. An omitted end is exclusive.
    const bool start_omitted = request.start_form == SliceStart::omitted;
    const bool end_omitted = request.end_form == SliceEnd::omitted;
    const bool start_exclusive = start_marker != 0;
    const bool end_exclusive = end_omitted || end_marker != 0;
    const std::int64_t start = start_omitted ? (ascending ? 0 : n - 1) : request.start;
    const std::int64_t end = end_omitted ? (ascending ? n : -1) : request.end;

    // Every explicit endpoint lies in [-1, len] (rules 2-4), so the
    // arithmetic below works on bounded values.
    if ((!start_omitted && (start < -1 || start > n)) ||
        (!end_omitted && (end < -1 || end > n)))
        return {{}, SliceStatus::out_of_bounds};
    if (strict_explicit_step_endpoints && request.step && !start_omitted && !end_omitted &&
        (ascending ? start > end : start < end))
        return {{}, SliceStatus::out_of_bounds};

    // The count: the j >= sigma with d*(s + j*k) <= d*e (inclusive end) or
    // < d*e (exclusive end), sigma = 1 for an exclusive start. With
    // m = |k| and D = d*(e - s), that is j*m <= D (or <= D - 1). D lies in
    // [-(len + 1), len + 1]; its magnitude is kept unsigned.
    const std::uint64_t magnitude = step < 0
        ? std::uint64_t(-(step + 1)) + 1u
        : static_cast<std::uint64_t>(step);
    const bool distance_negative = ascending ? end < start : start < end;
    std::uint64_t distance = ascending
        ? (end >= start ? std::uint64_t(end) - std::uint64_t(start)
                        : std::uint64_t(start) - std::uint64_t(end))
        : (start >= end ? std::uint64_t(start) - std::uint64_t(end)
                        : std::uint64_t(end) - std::uint64_t(start));
    // limit = D (inclusive) or D - 1 (exclusive), as a sign and magnitude.
    bool limit_negative = distance_negative;
    if (end_exclusive) {
        if (distance_negative) {
            ++distance;
        } else if (distance == 0) {
            limit_negative = true;
            distance = 1;
        } else {
            --distance;
        }
    }
    std::uint64_t reached = limit_negative ? 0 : distance / magnitude + 1u;
    const std::uint64_t skipped = start_exclusive ? 1u : 0u;
    const std::uint64_t count = reached > skipped ? reached - skipped : 0u;
    const bool empty = count == 0;

    // Rule 3: an inclusive end is a valid index, or len when empty.
    if (!end_omitted && !end_exclusive && !((end >= 0 && end < n) || (end == n && empty)))
        return {{}, SliceStatus::out_of_bounds};
    // Rule 4: an inclusive start is a valid index, or len when empty.
    if (!start_omitted && !start_exclusive &&
        !((start >= 0 && start < n) || (start == n && empty)))
        return {{}, SliceStatus::out_of_bounds};
    // Rule 5: with both sides inclusive the start does not pass the end.
    if (!start_exclusive && !end_exclusive && distance_negative)
        return {{}, SliceStatus::out_of_bounds};
    if (empty) return {{0, step, 0}, SliceStatus::ok};

    // first = s + sigma*k, a valid index here: compute it unsigned.
    const auto anchor = static_cast<std::uint64_t>(start);
    const std::uint64_t offset = start_exclusive ? magnitude : 0u;
    const std::uint64_t first = ascending ? anchor + offset : anchor - offset;
    const SliceIndexPlan plan{static_cast<std::int64_t>(first), step, count};
    // Rule 1: every selected index lies in [0, len). The rules above imply
    // it; it is checked at both ends of the progression all the same.
    const auto last = plan.index_at(count - 1);
    if (plan.first < 0 || plan.first >= n || last < 0 || last >= n)
        return {{}, SliceStatus::out_of_bounds};
    return {plan, SliceStatus::ok};
}

} // namespace quidra
