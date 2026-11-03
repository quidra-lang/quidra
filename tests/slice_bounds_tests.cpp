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
#include <optional>
#include <string>
#include <vector>

using namespace quidra;
static_assert(static_cast<int>(SliceDirection::ascending)==0&&static_cast<int>(SliceDirection::descending)==1&&static_cast<int>(SliceDirection::by_step_sign)==2&&static_cast<int>(SliceDirection::by_bounds)==3);

namespace {

constexpr auto I = SliceStart::inclusive;
constexpr auto SA = SliceStart::exclusive_ascending;
constexpr auto SD = SliceStart::exclusive_descending;
constexpr auto SO = SliceStart::omitted;
constexpr auto EI = SliceEnd::inclusive;
constexpr auto EA = SliceEnd::exclusive_ascending;
constexpr auto ED = SliceEnd::exclusive_descending;
constexpr auto EO = SliceEnd::omitted;

SliceRequest request(SliceStart start_form, std::int64_t start, SliceEnd end_form,
                     std::int64_t end, std::optional<std::int64_t> step = std::nullopt) {
    return SliceRequest{start_form, start, end_form, end, step};
}

void expect(std::uint64_t length, const SliceRequest& slice, std::vector<std::int64_t> indices) {
    const auto resolved = plan_slice(length, slice);
    CHECK(resolved);
    CHECK(resolved.plan.count == indices.size());
    for (std::uint64_t i = 0; i < resolved.plan.count; ++i)
        CHECK(resolved.plan.index_at(i) == indices[i]);
}

void expect_status(std::uint64_t length, const SliceRequest& slice, SliceStatus status) {
    CHECK(plan_slice(length, slice).status == status);
}

// The cases written out: the examples of the slice rules, the omitted forms, the
// empty slices of rules 3-5 and the extreme operands.
void written_cases() {
    expect(5, request(SO, 0, EO, 0), {0, 1, 2, 3, 4});
    expect(0, request(SO, 0, EO, 0), {});
    // An empty suffix at the exact length; an inclusive end at length is
    // out of bounds unless the slice is empty.
    expect(0, request(I, 0, EO, 0), {});
    expect(5, request(I, 5, EO, 0), {});
    expect(5, request(I, 5, EA, 5), {});
    expect(0, request(I, 0, EA, 0), {});
    expect(5, request(I, 5, EO, 0, 2), {});
    expect_status(5, request(I, 6, EO, 0), SliceStatus::out_of_bounds);
    expect_status(5, request(I, 5, EI, 5), SliceStatus::out_of_bounds);
    expect(5, request(I, 1, EI, 3), {1, 2, 3});
    expect(5, request(I, 1, EA, 3), {1, 2});
    expect(5, request(I, 0, EI, 4, 2), {0, 2, 4});
    // a[0:<N], a[0:<N:2], a[N>:0], a[N>:0:-2], a[10:>0:-2], a[10>:0:-2], a[0<:5].
    expect(6, request(I, 0, EA, 6), {0, 1, 2, 3, 4, 5});
    expect(6, request(I, 0, EA, 6, 2), {0, 2, 4});
    expect(6, request(SD, 6, EI, 0), {5, 4, 3, 2, 1, 0});
    expect(6, request(SD, 6, EI, 0, -2), {4, 2, 0});
    expect(11, request(I, 10, ED, 0, -2), {10, 8, 6, 4, 2});
    expect(11, request(SD, 10, EI, 0, -2), {8, 6, 4, 2, 0});
    expect(6, request(SA, 0, EI, 5), {1, 2, 3, 4, 5});
    // The reversal a[len(a)>:0] of an empty, a one-element and a longer array.
    expect(0, request(SD, 0, EI, 0), {});
    expect(1, request(SD, 1, EI, 0), {0});
    expect(5, request(SD, 5, EI, 0), {4, 3, 2, 1, 0});
    expect(5, request(SO, 0, EO, 0, -1), {4, 3, 2, 1, 0});
    // The anchor rule.
    expect(7, request(SA, 0, EI, 6, 2), {2, 4, 6});
    expect(7, request(SA, 1, EI, 6, 2), {3, 5});
    expect(7, request(I, 1, EI, 6, 2), {1, 3, 5});
    // A descending omitted start is the inclusive last index.
    expect(5, request(SO, 0, EO, 0, -2), {4, 2, 0});
    expect(5, request(SD, 5, EO, 0, -2), {3, 1});
    expect(5, request(SO, 0, EO, 0, -3), {4, 1});
    // Excluded endpoints just outside.
    expect(5, request(SA, -1, EI, 2), {0, 1, 2});
    expect(5, request(I, 2, ED, -1), {2, 1, 0});
    expect_status(5, request(SA, -2, EI, 2), SliceStatus::out_of_bounds);
    expect_status(5, request(SD, 6, EI, 0), SliceStatus::out_of_bounds);
    expect_status(5, request(SD, 6, EI, 0, -2), SliceStatus::out_of_bounds);
    // Rules 3 and 4: an inclusive endpoint of an empty slice may be len.
    expect(3, request(SD, 3, EI, 3), {});
    expect(5, request(I, 5, ED, 5), {});
    expect(5, request(I, 5, EA, 3), {});
    expect(5, request(SD, 4, EI, 5), {});
    expect(0, request(SA, 0, EI, 0), {});
    expect_status(0, request(SA, -1, EI, -1), SliceStatus::out_of_bounds);
    expect_status(5, request(SA, -1, EI, -1), SliceStatus::out_of_bounds);
    expect_status(3, request(SA, 2, EI, 4, 3), SliceStatus::out_of_bounds);
    // Rule 5: a start past the end is empty with an exclusive side.
    expect(10, request(I, 5, EA, 3), {});
    expect(10, request(SD, 3, EI, 5), {});
    expect(10, request(I, 5, EI, 3), {5, 4, 3});
    expect_status(10, request(I, 3, EI, 5, -1), SliceStatus::out_of_bounds);
    expect_status(10, request(I, 12, EA, 3), SliceStatus::out_of_bounds);
    // Empty results.
    expect(5, request(I, 2, EA, 2), {});
    expect(5, request(I, 2, ED, 2, -1), {});
    expect(5, request(SA, 2, EI, 2), {});
    expect(5, request(SD, 2, EI, 2), {});
    expect(5, request(SA, 2, EA, 3), {});
    expect(5, request(I, 4, EA, 1), {});
    expect(5, request(I, 3, EA, 3), {});
    // Two explicit unmarked endpoints choose the default direction. A
    // written step never flips, even when one endpoint is exclusive.
    expect(5, request(I, 4, EI, 0), {4, 3, 2, 1, 0});
    expect(5, request(I, 4, EI, 1), {4, 3, 2, 1});
    expect(5, request(I, 2, EI, 2), {2});
    expect(5, request(I, 4, EI, 0, -2), {4, 2, 0});
    expect_status(5, request(I, 4, EI, 0, 2), SliceStatus::out_of_bounds);
    expect_status(5, request(I, 0, EI, 4, -2), SliceStatus::out_of_bounds);
    expect_status(5, request(I, 4, EA, 0, 2), SliceStatus::out_of_bounds);
    expect_status(5, request(SD, 0, EI, 4, -2), SliceStatus::out_of_bounds);
    // Without an explicit step, direction markers retain their empty cases.
    expect(5, request(I, 4, EA, 0), {});
    expect(5, request(I, 0, ED, 4), {});
    // Steps that skip the end, and the extreme steps.
    expect(6, request(I, 1, EA, 6, 3), {1, 4});
    expect(6, request(I, 1, EA, 6, std::numeric_limits<std::int64_t>::max()), {1});
    expect(5, request(I, 2, EA, 4, std::numeric_limits<std::int64_t>::max()), {2});
    expect(5, request(I, 4, ED, 0, std::numeric_limits<std::int64_t>::min()), {4});
    // Direction and step errors.
    expect_status(5, request(I, 1, EI, 4, 0), SliceStatus::zero_step);
    expect_status(5, request(SA, 1, EI, 4, -1), SliceStatus::step_against_marker);
    expect_status(5, request(I, 1, ED, 4, 1), SliceStatus::step_against_marker);
    expect_status(5, request(SA, 0, ED, 4), SliceStatus::marker_conflict);
    expect_status(5, request(SD, 0, EA, 4), SliceStatus::marker_conflict);
    expect(5, request(I, 4, EI, 1), {4, 3, 2, 1});
    expect_status(5, request(I, 5, EI, 0), SliceStatus::out_of_bounds);
    // The positive-only tensor adapter keeps reversed exclusive empties;
    // opting out does not allow both-inclusive wrong-direction selections.
    const auto tensor_empty = plan_slice(5, request(I, 4, EA, 1, 1), false);
    CHECK(tensor_empty && tensor_empty.plan.count == 0 && tensor_empty.plan.step == 1);
    CHECK(plan_slice(5, request(I, 4, EI, 1, 1), false).status == SliceStatus::out_of_bounds);
    // Operands far outside, and the largest lengths.
    expect_status(5, request(I, std::numeric_limits<std::int64_t>::min(), EI, 2),
                  SliceStatus::out_of_bounds);
    expect_status(5, request(I, 0, EA, std::numeric_limits<std::int64_t>::max()),
                  SliceStatus::out_of_bounds);
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    expect_status(static_cast<std::uint64_t>(max), request(I, max - 1, ED, max, -1), SliceStatus::out_of_bounds);
    expect(static_cast<std::uint64_t>(max), request(SD, max, EI, max - 2), {max - 1, max - 2});
    expect(static_cast<std::uint64_t>(max), request(SA, -1, EI, 1), {0, 1});
    expect_status(static_cast<std::uint64_t>(max) + 1u, request(SO, 0, EO, 0),
                  SliceStatus::length_overflow);
}

// The brute-force oracle: the index set and the five bound rules of
// quidra/slice_bounds.hpp written directly, with each candidate index enumerated, against the plan,
// over every form pair, the omitted forms, steps 0 and +-1..3, lengths 0-6
// and every endpoint from -3 to len + 2.
enum class Outcome { selected, step_error, bounds_error };

struct Expected {
    Outcome outcome;
    std::vector<std::int64_t> indices;
};

Expected oracle(std::int64_t length, SliceStart start_form, std::int64_t s0, SliceEnd end_form,
                std::int64_t e0, std::optional<std::int64_t> k0) {
    const int sm = start_form == SA ? 1 : start_form == SD ? -1 : 0;
    const int em = end_form == EA ? 1 : end_form == ED ? -1 : 0;
    if (sm != 0 && em != 0 && sm != em) return {Outcome::step_error, {}};
    int d = sm != 0 ? sm : em;
    std::int64_t k = 0;
    if (k0) {
        if (*k0 == 0) return {Outcome::step_error, {}};
        const int sign = *k0 < 0 ? -1 : 1;
        if (d != 0 && d != sign) return {Outcome::step_error, {}};
        d = sign;
        k = *k0;
    } else {
        if (d == 0) d = start_form != SO && end_form != EO && s0 > e0 ? -1 : 1;
        k = d;
    }
    const bool s_omit = start_form == SO, e_omit = end_form == EO;
    if (k0 && !s_omit && !e_omit && (d > 0 ? s0 > e0 : s0 < e0))
        return {Outcome::bounds_error, {}};
    const std::int64_t s = s_omit ? (d > 0 ? 0 : length - 1) : s0;
    const std::int64_t e = e_omit ? (d > 0 ? length : -1) : e0;
    const bool s_excl = sm != 0;
    const bool e_excl = e_omit || em != 0;
    std::vector<std::int64_t> indices;
    for (std::int64_t j = s_excl ? 1 : 0;; ++j) {
        const std::int64_t i = s + j * k;
        if (!(e_excl ? d * i < d * e : d * i <= d * e)) break;
        indices.push_back(i);
        if (indices.size() > 100) break;
    }
    const bool empty = indices.empty();
    if (!s_omit && s_excl && (s < -1 || s > length)) return {Outcome::bounds_error, {}};
    if (!e_omit && e_excl && (e < -1 || e > length)) return {Outcome::bounds_error, {}};
    if (!e_omit && !e_excl && !((e >= 0 && e < length) || (e == length && empty)))
        return {Outcome::bounds_error, {}};
    if (!s_omit && !s_excl && !((s >= 0 && s < length) || (s == length && empty)))
        return {Outcome::bounds_error, {}};
    const bool passes = d > 0 ? s > e : s < e;
    if (passes && !s_excl && !e_excl) return {Outcome::bounds_error, {}};
    for (const auto i : indices)
        if (i < 0 || i >= length) return {Outcome::bounds_error, {}};
    return {Outcome::selected, indices};
}

void brute_force_oracle() {
    const SliceStart starts[] = {I, SA, SD, SO};
    const SliceEnd ends[] = {EI, EA, ED, EO};
    const std::optional<std::int64_t> steps[] = {std::nullopt, 1, -1, 2, -2, 3, -3, 0};
    long long cases = 0;
    for (std::int64_t length = 0; length <= 6; ++length)
        for (const auto start_form : starts)
            for (const auto end_form : ends)
                for (const auto step : steps)
                    for (std::int64_t s = -3; s <= length + 2; ++s)
                        for (std::int64_t e = -3; e <= length + 2; ++e) {
                            if (start_form == SO && s != -3) continue;
                            if (end_form == EO && e != -3) continue;
                            const auto expected = oracle(length, start_form, s, end_form, e, step);
                            const auto planned = plan_slice(
                                static_cast<std::uint64_t>(length),
                                request(start_form, s, end_form, e, step));
                            ++cases;
                            bool same = false;
                            switch (planned.status) {
                                case SliceStatus::ok: {
                                    std::vector<std::int64_t> indices;
                                    for (std::uint64_t i = 0; i < planned.plan.count; ++i)
                                        indices.push_back(planned.plan.index_at(i));
                                    same = expected.outcome == Outcome::selected &&
                                           expected.indices == indices;
                                    break;
                                }
                                case SliceStatus::marker_conflict:
                                case SliceStatus::zero_step:
                                case SliceStatus::step_against_marker:
                                    same = expected.outcome == Outcome::step_error;
                                    break;
                                case SliceStatus::out_of_bounds:
                                    same = expected.outcome == Outcome::bounds_error;
                                    break;
                                case SliceStatus::length_overflow:
                                    break;
                            }
                            if (!same) {
                                const auto text = slice_form_text(start_form, s, end_form, e,
                                                                  step.has_value(), step.value_or(0));
                                std::fprintf(stderr, "slice oracle mismatch: length %lld, [%.*s]\n",
                                             static_cast<long long>(length),
                                             static_cast<int>(text.size), text.data);
                                std::abort();
                            }
                        }
    CHECK(cases == 45920);
}

// The written form echoed in messages.
void written_forms() {
    CHECK(slice_form_text(I, 2, EA, 7).view() == "2:<7");
    CHECK(slice_form_text(I, 10, ED, 0, true, -2).view() == "10:>0:-2");
    CHECK(slice_form_text(SO, 0, EI, 7).view() == ":7");
    CHECK(slice_form_text(SD, 10, EI, 0, true, -2).view() == "10>:0:-2");
    CHECK(slice_form_text(SA, -1, EI, 3).view() == "-1<:3");
    CHECK(slice_form_text(SO, 0, EO, 0, true, -1).view() == "::-1");
    CHECK(slice_form_text(I, 2, EO, 0).view() == "2:");
    CHECK(slice_form_text(I, std::numeric_limits<std::int64_t>::min(), EA,
                          std::numeric_limits<std::int64_t>::max(), true,
                          std::numeric_limits<std::int64_t>::min()).view() ==
          "-9223372036854775808:<9223372036854775807:-9223372036854775808");
}

} // namespace

int main() {
    written_cases();
    brute_force_oracle();
    written_forms();
}
