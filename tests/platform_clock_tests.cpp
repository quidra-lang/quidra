// Exercise the production OS clocks and their checked integer conversions.
// Including the implementation keeps conversion-boundary tests private to it.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>

#if __has_include("../src/platform/clock.cpp")
#include "../src/platform/clock.cpp"

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void conversions() {
    using namespace quidra::platform::clock_detail;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t result = 0;
    expect(scale_ns(maximum, 1, 1, result) && result == maximum,
           "nanosecond conversion preserves the largest representable value");
    expect(scale_ns(maximum, 2, 2, result) && result == maximum,
           "a representable ratio must not fail on intermediate overflow");
    expect(scale_ns(maximum - 1, maximum - 1, maximum, result) &&
               result == maximum - 2,
           "fractional scaling remains exact at the uint64 boundary");
    expect(scale_ns(1, 1000000000, 3, result) && result == 333333333,
           "fractional clock ticks truncate below one nanosecond");
    expect(scale_ns(3, 1000000000, 3, result) && result == 1000000000,
           "three third-second ticks equal one second");
    expect(!scale_ns(maximum, 2, 1, result), "overflow cannot wrap to a timestamp");
    expect(!scale_ns(10, 1, 0, result), "invalid timebase cannot become a timestamp");
    expect(!scale_ns(10, 0, 1, result), "zero timebase numerator is rejected");
    expect(timespec_ns(18446744073LL, 709551615, result) && result == maximum,
           "seconds and nanoseconds accept the final uint64 boundary");
    expect(!timespec_ns(18446744073LL, 709551616, result),
           "seconds and nanoseconds reject overflow by one");
    expect(!timespec_ns(-1, 0, result), "negative seconds are rejected");
    expect(!timespec_ns(0, -1, result), "negative nanoseconds are rejected");
    expect(!timespec_ns(0, 1000000000, result), "unnormalized nanoseconds are rejected");
    expect(translate_start(1000, 2000, 5000) == 4000,
           "process creation uses its age at the clock pair");
    expect(translate_start(2000, 1000, 5000) == 0,
           "a future wall stamp is a failed signed delta");
    expect(translate_start(1000, 2000, 500) == 0,
           "a process age outside the monotonic domain cannot underflow");
    expect(translate_start(100, 110, 2000, 100) == 1000,
           "FILETIME delta units convert before monotonic subtraction");
    expect(translate_start(0, maximum, maximum, 100) == 0,
           "an overflowing FILETIME age is a conversion failure");
    expect(parse_linux_start_ticks(
               "42 (name ) with spaces) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 12345 99",
               result) && result == 12345,
           "Linux process-name parentheses do not shift the creation field");
    expect(!parse_linux_start_ticks("42 (short) S 1 2", result),
           "an incomplete process-stat query has no creation timestamp");
    expect(!parse_linux_start_ticks(
               "42 (bad) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 -1", result),
           "a negative process-stat creation field is rejected");
#if defined(__APPLE__) || defined(__linux__)
    expect(posix_clock_ns(static_cast<clockid_t>(-1)) == 0,
           "failed OS clock queries use the unavailable sentinel");
#endif
}

void os_clocks() {
    using namespace quidra::platform;
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    const auto before = monotonic_ns();
    const auto cpu_before = process_cpu_ns();
    // Delaying the first start query catches a first-call timestamp substitute.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto start = process_start_ns();
    const auto after = monotonic_ns();
    const auto cpu_after = process_cpu_ns();
    expect(before != 0 && after >= before, "monotonic clock is available and ordered");
    expect(after - before >= 1000000, "elapsed clock measures a short sleep in nanoseconds");
#ifdef _WIN32
    // GetProcessTimes can report zero until Windows accounts the first CPU
    // quantum. The bounded work below proves availability without mistaking
    // a legitimately coarse initial sample for an OS query failure.
    expect(cpu_after >= cpu_before, "process CPU clock is ordered");
#else
    expect(cpu_before != 0 && cpu_after >= cpu_before, "process CPU clock is available and ordered");
#endif
    expect(start != 0 && start <= before, "OS process creation predates the first clock query");
    expect(process_start_ns() == start, "process start is one immutable clock-pair conversion");
    // Windows process CPU accounting may advance at coarser intervals than
    // this short loop. Exercise actual CPU work until the OS clock ticks,
    // with a bounded deadline so a genuinely broken clock still fails.
    auto next_cpu = cpu_after;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (next_cpu <= cpu_after && std::chrono::steady_clock::now() < deadline) {
        volatile std::uint64_t work = 0;
        for (std::uint64_t i = 0; i != 100000; ++i) work = work + i;
        (void)work;
        next_cpu = process_cpu_ns();
    }
    expect(next_cpu > cpu_after, "process CPU clock advances with CPU work");
    auto previous = monotonic_ns();
    for (int i = 0; i != 100; ++i) {
        const auto next = monotonic_ns();
        expect(next >= previous, "successive monotonic reads do not go backwards");
        previous = next;
    }
#ifdef __APPLE__
    const auto raw_before = ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const auto native = monotonic_ns();
    const auto raw_after = ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    expect(raw_before <= native && native <= raw_after,
           "macOS monotonic clock uses the CLOCK_UPTIME_RAW host domain");
#endif
#else
    expect(monotonic_ns() == 0 && process_cpu_ns() == 0 && process_start_ns() == 0,
           "unsupported OSes explicitly report unavailable clocks");
#endif
}
} // namespace

int main() {
    conversions();
    os_clocks();
    if (failures != 0) return 1;
    std::cout << "platform clocks: ok\n";
    return 0;
}
#else
int main() {
    std::cerr << "FAIL: platform clock implementation is missing\n";
    return 1;
}
#endif
