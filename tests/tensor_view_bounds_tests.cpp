#include "tensor_view_bounds.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

// The runtime skips per-element initialization scans and copies when
// within_storage() proves a view stays inside its storage, and snapshots a view
// with one memcpy when dense_run() proves it is a single row-major run. These
// tests pin both proofs to the runtime's own storage_index() definition: an
// accepted view must never address an element outside storage (or out of
// order), and for the views the runtime actually creates (non-negative
// strides, no overflow) the bounds proof is exact.

namespace {

using quidra::tensor_view::dense_run;
using quidra::tensor_view::storage_index;
using quidra::tensor_view::within_storage;

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "tensor view bounds test failed: %s\n", what);
        ++failures;
    }
}

std::size_t element_count(const std::vector<long long>& shape) {
    std::size_t count = 1;
    for (const auto extent : shape) count *= static_cast<std::size_t>(extent);
    return count;
}

// The exact per-element check the runtime performed before the fast path.
bool every_index_in_storage(std::size_t offset,
                            const std::vector<long long>& shape,
                            const std::vector<long long>& strides,
                            std::size_t storage_count) {
    const auto count = element_count(shape);
    for (std::size_t logical = 0; logical < count; ++logical) {
        if (storage_index(offset, shape, strides, logical) >= storage_count) return false;
    }
    return true;
}

struct Case {
    const char* name;
    std::size_t offset;
    std::vector<long long> shape;
    std::vector<long long> strides;
    std::size_t storage_count;
    bool expected;
};

void check_named_cases() {
    constexpr auto big = std::numeric_limits<long long>::max();
    constexpr auto size_max = std::numeric_limits<std::size_t>::max();
    const std::vector<Case> cases = {
        {"contiguous, offset 0", 0, {2, 3}, {3, 1}, 6, true},
        {"contiguous, storage larger than view", 0, {2, 3}, {3, 1}, 7, true},
        {"contiguous slice with offset", 3, {1, 3}, {3, 1}, 6, true},
        {"slice ending exactly at storage end", 2, {4}, {1}, 6, true},
        {"slice one element past storage end", 3, {4}, {1}, 6, false},
        {"transpose", 0, {3, 2}, {1, 3}, 6, true},
        {"transpose of an offset view", 1, {3, 2}, {1, 3}, 6, false},
        {"stepped slice reaching the last element", 1, {3}, {2}, 6, true},
        {"stepped slice one past the end", 2, {3}, {2}, 6, false},
        {"singleton axes", 2, {1, 3, 1}, {99, 1, 7}, 5, true},
        {"singleton axis with a huge stride", 0, {1, 2}, {big, 1}, 2, true},
        {"broadcast-like zero stride", 4, {5, 2}, {0, 1}, 6, true},
        {"zero-sized tensor", 0, {0, 3}, {3, 1}, 0, true},
        {"zero-sized view past the storage end", 100, {2, 0}, {9, 1}, 4, true},
        {"rank-0 inside storage", 5, {}, {}, 6, true},
        {"rank-0 at storage end", 6, {}, {}, 6, false},
        {"empty storage, one element", 0, {1}, {1}, 0, false},
        {"negative stride is never proven", 3, {3}, {-1}, 6, false},
        {"negative extent is never proven", 0, {-1}, {1}, 6, false},
        {"rank mismatch is never proven", 0, {2, 2}, {2}, 6, false},
        {"stride product overflow", 0, {4, 2}, {big, 1}, size_max, false},
        {"strided largest index equals storage count", 0, {3, 2}, {big, 1}, size_max, false},
        {"offset overflow", size_max - 1, {3}, {1}, size_max, false},
        {"largest index equals size max", size_max - 2, {3}, {1}, size_max, false},
    };
    for (const auto& item : cases) {
        const bool proven = within_storage(
            item.offset, item.shape, item.strides, item.storage_count);
        if (proven != item.expected) {
            std::fprintf(stderr, "case '%s': within_storage returned %s\n",
                         item.name, proven ? "true" : "false");
            ++failures;
        }
    }
}

// Deterministic xorshift so failures reproduce.
std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

long long pick(std::uint64_t& state, long long low, long long high) {
    const auto span = static_cast<std::uint64_t>(high - low + 1);
    return low + static_cast<long long>(next_random(state) % span);
}

void check_random_views() {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    std::size_t accepted = 0;
    for (int iteration = 0; iteration < 100000; ++iteration) {
        const auto rank = static_cast<std::size_t>(pick(state, 0, 4));
        std::vector<long long> shape(rank);
        std::vector<long long> strides(rank);
        bool non_negative = true;
        for (std::size_t axis = 0; axis < rank; ++axis) {
            shape[axis] = pick(state, 0, 4);
            strides[axis] = pick(state, -3, 7);
            non_negative = non_negative && strides[axis] >= 0;
        }
        const auto offset = static_cast<std::size_t>(pick(state, 0, 24));
        const auto storage_count = static_cast<std::size_t>(pick(state, 0, 48));
        const bool proven = within_storage(offset, shape, strides, storage_count);
        const bool exact = every_index_in_storage(offset, shape, strides, storage_count);
        if (proven) ++accepted;
        // Soundness: an accepted view never addresses outside storage.
        if (proven && !exact) {
            std::fprintf(stderr, "unsound acceptance at iteration %d\n", iteration);
            ++failures;
            return;
        }
        // Completeness for the views the runtime creates.
        if (non_negative && proven != exact) {
            std::fprintf(stderr, "non-negative view not proven at iteration %d\n", iteration);
            ++failures;
            return;
        }
    }
    expect(accepted > 1000, "random views exercise the accepting path");
}

// dense_run() must imply storage_index(offset, ..., i) == offset + i.
bool addresses_one_run(const std::vector<long long>& shape,
                       const std::vector<long long>& strides) {
    const auto count = element_count(shape);
    for (std::size_t logical = 0; logical < count; ++logical) {
        if (storage_index(7, shape, strides, logical) != 7 + logical) return false;
    }
    return true;
}

std::vector<long long> row_major_strides(const std::vector<long long>& shape) {
    std::vector<long long> strides(shape.size(), 1);
    long long stride = 1;
    for (std::size_t axis = shape.size(); axis-- > 0;) {
        strides[axis] = stride;
        stride *= shape[axis];
    }
    return strides;
}

void check_dense_runs() {
    constexpr auto big = std::numeric_limits<long long>::max();
    expect(dense_run({}, {}), "rank-0 is a run");
    expect(dense_run({2, 3}, {3, 1}), "row-major matrix is a run");
    expect(dense_run({4}, {1}), "vector is a run");
    expect(!dense_run({3, 2}, {1, 3}), "transpose is not a run");
    expect(!dense_run({3}, {2}), "stepped slice is not a run");
    expect(!dense_run({2, 3}, {4, 1}), "row slice of a wider matrix is not a run");
    expect(dense_run({1, 3}, {99, 1}), "singleton axis stride is ignored");
    expect(dense_run({3, 1}, {1, 1}), "transposed singleton column is a run");
    expect(dense_run({1, 1}, {big, big}), "all-singleton view is a run");
    expect(dense_run({0, 3}, {5, 9}), "zero-sized view is a run");
    expect(!dense_run({-1}, {1}), "negative extent is never a run");
    expect(!dense_run({2}, {-1}), "negative stride is never a run");
    expect(!dense_run({2, 2}, {1}), "rank mismatch is never a run");
    expect(!dense_run({2, 3}, {3, 0}), "zero stride is not a run");

    std::uint64_t state = 0xD1B54A32D192ED03ULL;
    std::size_t accepted = 0;
    for (int iteration = 0; iteration < 100000; ++iteration) {
        const auto rank = static_cast<std::size_t>(pick(state, 0, 4));
        std::vector<long long> shape(rank);
        for (auto& extent : shape) extent = pick(state, 0, 4);
        auto strides = row_major_strides(shape);
        // Perturb some views so both answers are exercised.
        if (rank != 0 && pick(state, 0, 1) == 1) {
            const auto axis = static_cast<std::size_t>(pick(state, 0, static_cast<long long>(rank) - 1));
            strides[axis] = pick(state, 0, 9);
        }
        const bool run = dense_run(shape, strides);
        if (run) ++accepted;
        if (run && !addresses_one_run(shape, strides)) {
            std::fprintf(stderr, "dense_run accepted a non-run at iteration %d\n", iteration);
            ++failures;
            return;
        }
        if (strides == row_major_strides(shape) && !run) {
            std::fprintf(stderr, "dense_run rejected row-major strides at iteration %d\n", iteration);
            ++failures;
            return;
        }
    }
    expect(accepted > 1000, "random views exercise dense runs");
}

} // namespace

int main() {
    check_named_cases();
    check_random_views();
    check_dense_runs();
    // storage_index() keeps the runtime's historical definition.
    expect(storage_index(3, {2, 3}, {3, 1}, 4) == 7, "row-major storage index");
    expect(storage_index(0, {3, 2}, {1, 3}, 1) == 3, "transposed storage index");
    expect(storage_index(5, {}, {}, 0) == 5, "rank-0 storage index");
    expect(storage_index(0, {2, 0}, {1, 1}, 0) == 0, "zero extent storage index");
    return failures == 0 ? 0 : 1;
}
