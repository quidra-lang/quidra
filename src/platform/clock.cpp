#include "platform/clock.hpp"

#include <charconv>
#include <limits>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fstream>
#include <iterator>
#include <string>
#include <time.h>
#include <unistd.h>
#endif

namespace quidra::platform {
namespace clock_detail {

constexpr std::uint64_t ns_per_second = 1000000000;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

// Exact floor(value * numerator / denominator), without floating point or
// overflowing the intermediate product. The uncommon wide fraction path also
// works on Windows compilers without a uint128 integer type.
bool scale_ns(std::uint64_t value, std::uint64_t numerator,
              std::uint64_t denominator, std::uint64_t& result) {
    if (numerator == 0 || denominator == 0) return false;
    const auto whole = value / denominator;
    const auto remainder = value % denominator;
    if (whole > maximum / numerator) return false;
    const auto integral = whole * numerator;
    std::uint64_t fraction = 0;
    if (remainder <= maximum / numerator) {
        fraction = remainder * numerator / denominator;
    } else {
        std::uint64_t residue = 0;
        for (int bit = 63; bit >= 0; --bit) {
            fraction *= 2;
            if (residue >= denominator - residue) {
                residue -= denominator - residue;
                ++fraction;
            } else {
                residue *= 2;
            }
            if (((numerator >> bit) & 1) != 0) {
                if (residue >= denominator - remainder) {
                    residue -= denominator - remainder;
                    ++fraction;
                } else {
                    residue += remainder;
                }
            }
        }
    }
    if (fraction > maximum - integral) return false;
    result = integral + fraction;
    return true;
}

bool timespec_ns(std::int64_t seconds, std::int64_t nanoseconds,
                 std::uint64_t& result) {
    if (seconds < 0 || nanoseconds < 0 ||
        static_cast<std::uint64_t>(nanoseconds) >= ns_per_second) return false;
    std::uint64_t integral = 0;
    if (!scale_ns(static_cast<std::uint64_t>(seconds), ns_per_second, 1, integral) ||
        static_cast<std::uint64_t>(nanoseconds) > maximum - integral) return false;
    result = integral + static_cast<std::uint64_t>(nanoseconds);
    return true;
}

// Compare before subtracting: a negative wall delta or an age outside the
// destination clock's domain is unavailable, never an unsigned wraparound.
std::uint64_t translate_start(std::uint64_t start_reference,
                              std::uint64_t now_reference,
                              std::uint64_t now_monotonic,
                              std::uint64_t unit_ns = 1) {
    if (start_reference > now_reference) return 0;
    std::uint64_t age = 0;
    if (!scale_ns(now_reference - start_reference, unit_ns, 1, age) ||
        age >= now_monotonic) return 0;
    return now_monotonic - age;
}

// Field 2 (comm) may itself contain spaces, parentheses and newlines. Field 22
// follows the final closing parenthesis, not the first whitespace token.
bool parse_linux_start_ticks(std::string_view text, std::uint64_t& result) {
    const auto close = text.rfind(')');
    if (close == std::string_view::npos) return false;
    text.remove_prefix(close + 1);
    for (int field = 3; field <= 22; ++field) {
        const auto begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return false;
        text.remove_prefix(begin);
        const auto end = text.find_first_of(" \t\r\n");
        const auto token = text.substr(0, end);
        if (field == 22) {
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
        }
        if (end == std::string_view::npos) return false;
        text.remove_prefix(end);
    }
    return false;
}

#if defined(__APPLE__) || defined(__linux__)
std::uint64_t posix_clock_ns(clockid_t clock) {
    struct timespec value {};
    std::uint64_t result = 0;
    if (::clock_gettime(clock, &value) != 0 ||
        !timespec_ns(value.tv_sec, value.tv_nsec, result)) return 0;
    return result;
}
#endif

#ifdef _WIN32
std::uint64_t filetime_ticks(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
           value.dwLowDateTime;
}

std::uint64_t qpc_frequency() {
    static const auto frequency = []() -> std::uint64_t {
        LARGE_INTEGER value{};
        if (!::QueryPerformanceFrequency(&value) || value.QuadPart <= 0) return 0;
        return static_cast<std::uint64_t>(value.QuadPart);
    }();
    return frequency;
}

bool precise_filetime(FILETIME& value) {
    using Query = VOID(WINAPI*)(LPFILETIME);
    static const auto query = []() -> Query {
        const auto kernel = ::GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return nullptr;
        return reinterpret_cast<Query>(::GetProcAddress(kernel, "GetSystemTimePreciseAsFileTime"));
    }();
    // This precise wall pair requires Windows 8+. Older systems explicitly
    // report an unavailable start rather than substituting a coarse epoch.
    if (!query) return false;
    query(&value);
    return true;
}
#endif

std::uint64_t query_process_start() {
#if defined(__APPLE__)
    struct kinfo_proc info {};
    std::size_t size = sizeof(info);
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
    if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size != sizeof(info)) return 0;
    const auto created = info.kp_proc.p_starttime;
    std::uint64_t start_wall = 0;
    if (created.tv_usec < 0 || created.tv_usec >= 1000000 ||
        !timespec_ns(created.tv_sec, created.tv_usec * 1000, start_wall)) return 0;
    const auto before = monotonic_ns();
    const auto reference = posix_clock_ns(CLOCK_REALTIME);
    const auto after = monotonic_ns();
    if (before == 0 || reference == 0 || after < before) return 0;
    return translate_start(start_wall, reference, before + (after - before) / 2);
#elif defined(__linux__)
    // The kernel exports start_boottime in _SC_CLK_TCK units. Using BOOTTIME
    // as the reference avoids /proc/stat's coarse/adjustable wall epoch.
    std::ifstream stat("/proc/self/stat", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(stat), {}};
    std::uint64_t ticks = 0;
    const auto ticks_per_second = ::sysconf(_SC_CLK_TCK);
    std::uint64_t start_boot = 0;
    if (!stat || ticks_per_second <= 0 || !parse_linux_start_ticks(text, ticks) ||
        !scale_ns(ticks, ns_per_second, static_cast<std::uint64_t>(ticks_per_second), start_boot)) return 0;
    const auto before = monotonic_ns();
    const auto reference = posix_clock_ns(CLOCK_BOOTTIME);
    const auto after = monotonic_ns();
    if (before == 0 || reference == 0 || after < before) return 0;
    return translate_start(start_boot, reference, before + (after - before) / 2);
#elif defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto before = monotonic_ns();
    FILETIME reference{};
    if (!precise_filetime(reference)) return 0;
    const auto after = monotonic_ns();
    if (before == 0 || after < before) return 0;
    // Subtract in FILETIME's 100ns domain first: scaling the 1601 epoch into
    // ns would unnecessarily shorten the representable calendar range.
    return translate_start(filetime_ticks(created), filetime_ticks(reference),
                           before + (after - before) / 2, 100);
#else
    return 0;
#endif
}

} // namespace clock_detail

std::uint64_t monotonic_ns() {
#if defined(__APPLE__)
    // This is CLOCK_UPTIME_RAW's implementation/domain: mach_absolute_time
    // scaled by mach_timebase_info. Do the scaling here to check overflow in
    // place of libc's unchecked ticks * numer intermediate product.
    static const auto timebase = [] {
        mach_timebase_info_data_t value{};
        if (::mach_timebase_info(&value) != KERN_SUCCESS) value = {};
        return value;
    }();
    std::uint64_t result = 0;
    if (!clock_detail::scale_ns(::mach_absolute_time(), timebase.numer, timebase.denom, result)) return 0;
    return result;
#elif defined(__linux__)
    return clock_detail::posix_clock_ns(CLOCK_MONOTONIC);
#elif defined(_WIN32)
    LARGE_INTEGER value{};
    std::uint64_t result = 0;
    if (!::QueryPerformanceCounter(&value) || value.QuadPart < 0 ||
        !clock_detail::scale_ns(static_cast<std::uint64_t>(value.QuadPart),
                               clock_detail::ns_per_second, clock_detail::qpc_frequency(), result)) return 0;
    return result;
#else
    return 0;
#endif
}

std::uint64_t process_cpu_ns() {
#if defined(__APPLE__) || defined(__linux__)
    return clock_detail::posix_clock_ns(CLOCK_PROCESS_CPUTIME_ID);
#elif defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto kernel_ticks = clock_detail::filetime_ticks(kernel);
    const auto user_ticks = clock_detail::filetime_ticks(user);
    std::uint64_t result = 0;
    if (user_ticks > clock_detail::maximum - kernel_ticks ||
        !clock_detail::scale_ns(kernel_ticks + user_ticks, 100, 1, result)) return 0;
    return result;
#else
    return 0;
#endif
}

std::uint64_t process_start_ns() {
    static const auto start = clock_detail::query_process_start();
    return start;
}

} // namespace quidra::platform
