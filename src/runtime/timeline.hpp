#pragma once

// Optional runtime observation. Timestamps are nanoseconds in platform's
// monotonic host domain; GPU producers must align their timestamps to it.
// Recording never submits GPU work or synchronizes a stream. A generated
// program must claim the report at RuntimeInit; compiler-only hooks cannot.
#include "runtime_counters.hpp"

#include <atomic>
#include <cstdint>

namespace quidra::timeline {

enum class Point : std::uint8_t {
    ProcessStart, RuntimeInit, FirstStatement, DeviceInitBegin, DeviceInitEnd,
    StepBoundary, TimeMark, ExitBegin, ExitEnd
};

struct HostEvent {
    Point point{};
    std::uint32_t step{};
    std::uint64_t host_ns{}, cpu_ns{};
    counters::SourceSite site{};
};
struct WaitInterval {
    std::uint32_t step{};
    counters::WaitReason reason{};
    std::uint64_t begin_ns{}, end_ns{};
    counters::SourceSite site{};
};
struct GpuInterval {
    std::int32_t device{};
    std::uint64_t serial{};
    std::uint32_t commit_step{};
    std::uint64_t commit_ns{}, scheduled_begin_ns{}, scheduled_end_ns{};
    std::uint64_t gpu_begin_ns{}, gpu_end_ns{};
    std::uint32_t ops{};
    std::uint64_t bytes{};
    counters::SourceSite site{};
};
// Separate from command-buffer intervals: a commit can span several steps.
// Input only; stage sampling/event insertion belongs to the device backend.
struct GpuStepInterval {
    std::int32_t device{};
    std::uint64_t serial{};
    std::uint32_t step{};
    std::uint64_t gpu_begin_ns{}, gpu_end_ns{};
};
// Instrumentation cost is separate from the physical-work counters.
struct GpuSampling {
    std::int32_t device{};
    std::uint64_t serial{};
    const char* setup_gap{};
    std::uint64_t sample_buffer_allocations{}, compute_encoders{}, blit_encoders{};
    std::uint64_t sampled_passes{}, additional_compute_encoders{};
    std::uint64_t calibration_queries{}, calibration_host_ns{};
    std::uint64_t before_cpu_ns{}, before_gpu_ticks{}, after_cpu_ns{}, after_gpu_ticks{};
    std::uint64_t resolved_intervals{}, missing_intervals{};
};
struct GpuSamplingGap {
    std::int32_t device{};
    std::uint64_t serial{};
    std::uint32_t step{};
    const char* reason{};
    bool compute{}, additional_encoder{};
    // Coarse kernel/source identity can coexist with a valid step duration.
    bool timing_incomplete{true};
};
struct WarmupInterval {
    std::int32_t device{};
    std::uint32_t function{};
    std::uint64_t begin_ns{}, end_ns{};
    std::uint32_t step{};
    counters::SourceSite site{};
};
struct CompileInterval {
    std::int32_t device{};
    std::uint32_t pipeline{};
    bool timed_path{};
    std::uint64_t begin_ns{}, end_ns{};
    std::uint32_t step{};
    counters::SourceSite site{};
};
// Typed M7 input. Zero numeric fields or empty text mean unavailable. This
// module observes supplied capabilities; it does not query or select devices.
struct DeviceCapabilities {
    std::int32_t device{};
    const char* backend{};
    const char* architecture{};
    const char* device_key{};
    std::uint64_t compute_units{}, max_threads_per_group{}, threadgroup_memory_bytes{};
    std::uint64_t simd_width{}, working_set_bytes{};
    bool unified_memory{};
    const char* driver{};
    const char* os_build{};
    // A known false unified-memory capability is distinct from unavailable.
    bool unified_memory_known{};
};

namespace detail {
extern std::atomic<bool> enabled;
extern std::atomic<std::uint32_t> step;
extern std::atomic<bool> time_steps;
void mark_program_started_slow() noexcept;
void note_point_slow(Point, const counters::SourceSite*) noexcept;
void note_host_slow(const HostEvent&) noexcept;
void note_wait_slow(const WaitInterval&) noexcept;
void note_wait_slow(counters::WaitReason, std::uint64_t, std::uint64_t,
                    const counters::SourceSite*) noexcept;
void note_gpu_slow(const GpuInterval&) noexcept;
void note_gpu_step_slow(const GpuStepInterval&) noexcept;
void note_gpu_sampling_slow(const GpuSampling&) noexcept;
void note_gpu_sampling_gap_slow(const GpuSamplingGap&) noexcept;
void note_warmup_slow(const WarmupInterval&) noexcept;
void note_compile_slow(const CompileInterval&) noexcept;
void note_device_slow(const DeviceCapabilities&) noexcept;
}

inline bool enabled() noexcept {
    return detail::enabled.load(std::memory_order_relaxed);
}
inline std::uint32_t current_step() noexcept {
    return detail::step.load(std::memory_order_relaxed);
}
inline bool time_marks_are_steps() noexcept {
    return enabled() && detail::time_steps.load(std::memory_order_relaxed);
}
inline void mark_program_started() noexcept {
    if (!enabled()) return;
    detail::mark_program_started_slow();
}
// Called by the program-runtime registration at library initialization. It
// retains the startup stamps without claiming a report: the compiler links
// this runtime too. A later generated-program entry publishes them once.
void remember_runtime_initialized() noexcept;
inline void note_point(Point point, const counters::SourceSite* site = nullptr) noexcept {
    if (!enabled()) return;
    detail::note_point_slow(point, site);
}
// Already-stamped producer input uses the same recording path. A boundary
// must name the current step; it is recorded before that step advances.
inline void note_host(const HostEvent& event) noexcept {
    if (!enabled()) return;
    detail::note_host_slow(event);
}
inline void note_wait(const WaitInterval& interval) noexcept {
    if (!enabled()) return;
    detail::note_wait_slow(interval);
}
inline void note_wait(counters::WaitReason reason, std::uint64_t begin_ns,
                      std::uint64_t end_ns, const counters::SourceSite* site = nullptr) noexcept {
    if (!enabled()) return;
    detail::note_wait_slow(reason, begin_ns, end_ns, site);
}
inline void note_gpu(const GpuInterval& interval) noexcept {
    if (!enabled()) return;
    detail::note_gpu_slow(interval);
}
inline void note_gpu_step(const GpuStepInterval& interval) noexcept {
    if (!enabled()) return;
    detail::note_gpu_step_slow(interval);
}
inline void note_gpu_sampling(const GpuSampling& sampling) noexcept {
    if (!enabled()) return;
    detail::note_gpu_sampling_slow(sampling);
}
inline void note_gpu_sampling_gap(const GpuSamplingGap& gap) noexcept {
    if (!enabled()) return;
    detail::note_gpu_sampling_gap_slow(gap);
}
inline void note_warmup(const WarmupInterval& interval) noexcept {
    if (!enabled()) return;
    detail::note_warmup_slow(interval);
}
inline void note_compile(const CompileInterval& interval) noexcept {
    if (!enabled()) return;
    detail::note_compile_slow(interval);
}
inline void note_device(const DeviceCapabilities& capabilities) noexcept {
    if (!enabled()) return;
    detail::note_device_slow(capabilities);
}

// Call after the final drain, including before an immediate failure exit.
// Normal library shutdown also flushes. Exactly one document is attempted;
// repeat calls return its write result and late callbacks cannot append to it.
// Report/storage failures never replace the program's result or exception.
bool flush_report() noexcept;

} // namespace quidra::timeline
