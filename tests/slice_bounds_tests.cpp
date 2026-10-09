#include "quidra/slice_bounds.hpp"
#include <cstdio>
#include <cstdlib>

// The CI and release builds compile with NDEBUG. Keep these bounds checks
// active there: assert() would turn this entire test into a no-op.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "slice bounds check failed at %s:%d: %s\n", \
                     __FILE__, __LINE__, #condition); \
        std::abort(); \
    } \
} while (false)
#include <cstdint>
#include <limits>
#include <vector>

using namespace quidra;

static void expect(std::uint64_t length, const SliceRequest& request,
                   std::vector<std::int64_t> indices) {
    const auto resolved = plan_slice(length, request);
    CHECK(resolved);
    CHECK(resolved.plan.count == indices.size());
    for (std::uint64_t i = 0; i < resolved.plan.count; ++i)
        CHECK(resolved.plan.index_at(i) == indices[i]);
}

int main() {
    expect(5, {}, {0, 1, 2, 3, 4});
    expect(0, {}, {});
    // The omitted-end form permits an empty suffix at the exact length,
    // but an explicitly inclusive end at length remains out of bounds.
    expect(0, SliceRequest{.start=0}, {});
    expect(5, SliceRequest{.start=5}, {});
    expect(5, SliceRequest{.start=5, .end=5, .exclude_end=true,
                            .end_marker=SliceDirection::ascending}, {});
    expect(0, SliceRequest{.start=0, .end=0, .exclude_end=true,
                            .end_marker=SliceDirection::ascending}, {});
    expect(5, SliceRequest{.start=5, .step=2}, {});
    CHECK(plan_slice(5, SliceRequest{.start=6}).status ==
           SliceStatus::endpoint_out_of_bounds);
    CHECK(plan_slice(5, SliceRequest{.start=5, .end=5}).status ==
           SliceStatus::endpoint_out_of_bounds);
    expect(5, SliceRequest{.start=1, .end=3}, {1, 2, 3});
    expect(5, SliceRequest{.start=1, .end=3, .exclude_end=true,
                           .end_marker=SliceDirection::ascending}, {1, 2});
    expect(5, SliceRequest{.start=0, .end=4, .step=2}, {0, 2, 4});
    expect(6, SliceRequest{.start=0, .end=5, .exclude_start=true,
                           .start_marker=SliceDirection::ascending}, {1, 2, 3, 4, 5});
    expect(5, SliceRequest{.start=5, .end=0, .exclude_start=true,
                           .start_marker=SliceDirection::descending}, {4, 3, 2, 1, 0});
    expect(11, SliceRequest{.start=10, .end=0, .step=-2,
                            .exclude_end=true, .end_marker=SliceDirection::descending}, {10, 8, 6, 4, 2});
    expect(11, SliceRequest{.start=10, .end=0, .step=-2,
                            .exclude_start=true, .start_marker=SliceDirection::descending}, {8, 6, 4, 2, 0});
    expect(5, SliceRequest{.step=-1}, {4, 3, 2, 1, 0});
    expect(5, SliceRequest{.start=3, .end=3, .exclude_end=true,
                            .end_marker=SliceDirection::ascending}, {});
    expect(5, SliceRequest{.start=4, .end=1, .exclude_start=true,
                            .start_marker=SliceDirection::ascending}, {});
    expect(5, SliceRequest{.start=0, .end=5, .exclude_end=true,
                            .end_marker=SliceDirection::ascending}, {0, 1, 2, 3, 4});
    expect(5, SliceRequest{.start=0, .end=5, .step=2, .exclude_end=true,
                            .end_marker=SliceDirection::ascending}, {0, 2, 4});
    expect(6, SliceRequest{.start=1, .end=6, .step=3, .exclude_end=true,
                            .end_marker=SliceDirection::ascending}, {1, 4});
    expect(6, SliceRequest{.start=1, .end=6,
                            .step=std::numeric_limits<std::int64_t>::max(),
                            .exclude_end=true, .end_marker=SliceDirection::ascending}, {1});

    expect(5, SliceRequest{.start=-1, .end=3, .exclude_start=true,
                            .start_marker=SliceDirection::ascending}, {0, 1, 2, 3});
    expect(5, SliceRequest{.start=2, .end=4, .step=std::numeric_limits<std::int64_t>::max(),
                            .exclude_end=true}, {2});
    expect(5, SliceRequest{.start=4, .end=0,
                            .step=std::numeric_limits<std::int64_t>::min(),
                            .exclude_end=true}, {4});
    CHECK(plan_slice(5, SliceRequest{.start=1, .end=4, .step=0}).status ==
           SliceStatus::zero_step);
    CHECK(plan_slice(5, SliceRequest{.start=4, .end=1}).status ==
           SliceStatus::empty_inclusive_range);
    CHECK(plan_slice(5, SliceRequest{.start=5, .end=0}).status ==
           SliceStatus::endpoint_out_of_bounds);
    CHECK(plan_slice(5, SliceRequest{.start=-2, .end=4, .exclude_start=true}).status ==
           SliceStatus::endpoint_out_of_bounds);
    CHECK(plan_slice(5, SliceRequest{.start=1, .end=4, .step=-1,
           .start_marker=SliceDirection::ascending}).status ==
           SliceStatus::direction_conflict);
    CHECK(plan_slice(5, SliceRequest{.start=0, .end=4, .exclude_start=true,
           .start_marker=SliceDirection::ascending, .end_marker=SliceDirection::descending}).status ==
           SliceStatus::direction_conflict);
    expect(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
           SliceRequest{.start=std::numeric_limits<std::int64_t>::max()-1,
                        .end=std::numeric_limits<std::int64_t>::max(),
                        .step=-1, .exclude_end=true,
                        .end_marker=SliceDirection::descending}, {});
    CHECK(plan_slice(std::uint64_t(std::numeric_limits<std::int64_t>::max())+1u, {}).status ==
           SliceStatus::length_overflow);
}
