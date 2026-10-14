#pragma once

// Optional host ranges and producer-supplied GPU timings. Hardware sampling,
// split encoders and capture are device-backend responsibilities.
// Timestamps are nanoseconds in platform's monotonic host domain. Producers
// must align backend GPU timestamps to that domain before recording them.
#include "runtime/timeline.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::profile {
namespace metal { struct Status; }
// Published only by generated-program entry; all views remain valid until
// process shutdown. Capture is independent of profiling and timeline output.
struct CaptureConfiguration {
    std::uint64_t requested_step{};
    std::string output_path;
    std::vector<std::string> excluded_storage;
    std::vector<std::string_view> excluded_paths;
};
bool capture_requested() noexcept;
const CaptureConfiguration* capture_configuration() noexcept;
std::uint64_t capture_current_step() noexcept;
void advance_capture_step() noexcept;
bool capture_time_marks_are_steps() noexcept;
void note_capture(const metal::Status&) noexcept;
using CaptureFinalizer = void (*)(bool) noexcept;
void set_capture_finalizer(CaptureFinalizer) noexcept;
struct ProfileEvent {
    enum class Kind : std::uint8_t {
        HostOp, Dispatch, CommandBuffer, Copy, Fill, Alloc, Free, Sync,
        PackageRange, PlanRun, Replay, Statement
    } kind{Kind::HostOp};
    counters::SourceSite site{};
    std::int32_t device{-1};
    std::uint32_t stream{};
    std::uint64_t host_begin_ns{}, host_end_ns{}, gpu_begin_ns{}, gpu_end_ns{};
    std::uint64_t bytes{}, kernel_hash{}, host_thread{};
    // Copied synchronously when recording; caller storage need not outlive it.
    const char* owner{};
    const char* name{};
    const char* kernel{};
    // Containers retain enclosing Chrome spans, but never contribute to GPU
    // activity totals. Automatic uses the typed kind and device, never names.
    enum class GpuRole : std::uint8_t { Automatic, None, Activity, Container };
    GpuRole gpu_role{GpuRole::Automatic};
};
using DeviceCapabilities = timeline::DeviceCapabilities;
namespace detail {
extern std::atomic<bool> enabled;
extern std::atomic<bool> split;
extern std::atomic<bool> capture_configured;
extern std::atomic<std::uint64_t> capture_step;
void mark_program_started_slow() noexcept;
void record_slow(const ProfileEvent&) noexcept;
void statement_changed_slow(const counters::SourceSite*) noexcept;
std::uint64_t range_begin_slow(const char*, const char*, std::int32_t,
                              ProfileEvent::Kind, const counters::SourceSite*) noexcept;
void range_end_slow(std::uint64_t) noexcept;
void note_device_slow(const DeviceCapabilities&) noexcept;
std::uint64_t current_thread_slow() noexcept;
}
inline bool enabled() noexcept { return detail::enabled.load(std::memory_order_relaxed); }
inline bool split_enabled() noexcept {
    return enabled() && detail::split.load(std::memory_order_relaxed);
}
inline void mark_program_started() noexcept {
    if (!enabled() && !detail::capture_configured.load(std::memory_order_relaxed)) return;
    detail::mark_program_started_slow();
}
// Initialize configuration independently of translation-unit constructor
// order, while leaving report ownership with the generated-program entry.
void configure_runtime() noexcept;
inline void record(const ProfileEvent& event) noexcept {
    if (!enabled()) return;
    detail::record_slow(event);
}
// A null site ends the calling thread's active statement attribution.
inline void statement_changed(const counters::SourceSite* site) noexcept {
    if (!enabled()) return;
    detail::statement_changed_slow(site);
}
inline std::uint64_t current_thread() noexcept {
    if (!enabled()) return 0;
    return detail::current_thread_slow();
}
inline std::uint64_t range_begin(const char* owner, const char* name,
    std::int32_t device = -1, ProfileEvent::Kind kind = ProfileEvent::Kind::HostOp,
    const counters::SourceSite* site = nullptr) noexcept {
    if (!enabled()) return 0;
    return detail::range_begin_slow(owner, name, device, kind, site);
}
inline void range_end(std::uint64_t token) noexcept {
    if (token == 0) return;
    if (!enabled()) return;
    detail::range_end_slow(token);
}
inline void note_device(const DeviceCapabilities& device) noexcept {
    if (!enabled()) return;
    detail::note_device_slow(device);
}
class Range {
public:
    Range(const char* owner, const char* name, std::int32_t device = -1,
          ProfileEvent::Kind kind = ProfileEvent::Kind::HostOp) noexcept
        : token_(range_begin(owner, name, device, kind)) {}
    ~Range() { if (token_ != 0) range_end(token_); }
    Range(const Range&) = delete;
    Range& operator=(const Range&) = delete;
private:
    std::uint64_t token_{};
};
// After final drain, or before immediate termination. One document is attempted;
// repeated calls retain that write result and late producers cannot append.
bool flush_report() noexcept;
} // namespace quidra::profile
