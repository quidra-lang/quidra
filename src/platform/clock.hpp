#pragma once

#include <cstdint>

namespace quidra::platform {

// Nanoseconds in the native monotonic host-timestamp domain: macOS
// CLOCK_UPTIME_RAW (mach absolute time), Linux CLOCK_MONOTONIC, Windows QPC.
// macOS/Linux exclude system suspend; Windows QPC includes it. None follows
// discontinuous wall-clock changes. Linux MONOTONIC may be frequency-adjusted.
// Zero means unavailable, failed OS query, or unrepresentable conversion.
std::uint64_t monotonic_ns();

// Sum of user and system CPU time for all threads of this process, in ns.
// This is accumulated CPU time, not elapsed time; it can exceed elapsed time.
// Zero also covers unavailable/query/conversion failure.
std::uint64_t process_cpu_ns();

// Actual OS process-creation stamp translated into monotonic_ns()'s domain.
// The first call captures one reference-clock/monotonic pair and caches even
// failure. RuntimeInit must call this before timed work. Never substitutes the
// first-call time for missing creation data. Zero means unavailable or invalid.
// macOS uses sysctl KERN_PROC_PID wall time, Linux /proc/self/stat boot-time
// ticks, Windows GetProcessTimes FILETIME. Precision is limited by those stamps
// and pairing latency. Windows creation conversion requires Windows 8+'s
// precise FILETIME pair; older Windows returns zero for this function.
// A single pair cannot reconstruct a historical wall jump
// or suspend between process creation and the pair on macOS/Linux. Call early;
// subsequent changes/suspend do not rebase the cached start. Unsupported OSes
// return zero rather than a first-call approximation.
std::uint64_t process_start_ns();

} // namespace quidra::platform
