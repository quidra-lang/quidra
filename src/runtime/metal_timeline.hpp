#pragma once

#include "runtime/timeline.hpp"
#include "runtime/profile.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace quidra::timeline::metal {

enum class Gap : std::uint8_t {
    None, Disabled, UnsupportedOS, InvalidHandle, DeviceMismatch,
    UnsupportedStageSampling, TimestampCounterMissing, AllocationFailed,
    CapacityExceeded, CalibrationFailed, EncoderSamplingFailed,
    CommandNotCompleted, FailedCommandBuffer, ResolveFailed, MalformedResult,
    CounterError, UnorderedSamples, AlignmentFailed, StorageFailed, Count
};
const char* name(Gap gap) noexcept;
struct ClockSample { std::uint64_t cpu_ns{}, gpu_ticks{}; };
// CPU correlation is already ns in the mach absolute host domain. Reject
// reversal, zero spans and extrapolation; truncate fractional ns downward.
bool align_timestamp(ClockSample before, ClockSample after, std::uint64_t gpu_ticks,
                     std::uint64_t& host_ns) noexcept;

struct Statistics {
    Gap setup_gap{Gap::None};
    std::uint64_t sample_buffer_allocations{}, compute_encoders{}, blit_encoders{};
    std::uint64_t sampled_passes{}, additional_compute_encoders{};
    std::uint64_t calibration_queries{}, calibration_host_ns{};
    std::uint64_t resolved_intervals{}, missing_intervals{};
    ClockSample before{}, after{};
    std::array<std::uint64_t, static_cast<std::size_t>(Gap::Count)> gaps{};
};
struct StageResult {
    GpuStepInterval interval;
    Gap gap{Gap::None};
    bool compute{}, additional_encoder{};
    std::uint64_t pass_index{};
};
using StageObserver = void (*)(void*, const StageResult&) noexcept;
class Sampler;
class Batch;
struct Creation { std::shared_ptr<Sampler> sampler; Gap gap{Gap::None}; };
struct CommandCreation { std::shared_ptr<Batch> batch; Gap gap{Gap::None}; };
namespace detail {
Creation make_sampler_slow(void* native_device, std::int32_t device) noexcept;
}

class Sampler {
public:
    ~Sampler();
    bool supported() const noexcept;
    Gap setup_gap() const noexcept;
    CommandCreation begin_command(void* native_command_buffer, std::size_t max_passes = 64,
                                  StageObserver observer = nullptr, void* context = nullptr) const noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Sampler(std::shared_ptr<Impl> impl);
    friend class Batch;
    friend Creation detail::make_sampler_slow(void*, std::int32_t) noexcept;
};

class Batch {
public:
    ~Batch();
    bool sampling_active() const noexcept;
    // Whether the most recently created compute encoder has end sampling.
    // Root also checks that it is still open and no package hold owns it.
    bool should_split_compute() const noexcept;
    // Return an autoreleased native encoder, as Metal's ordinary API does.
    // Caller retains/ends it as before. The helper never ends any encoder.
    // Tag additional only for an actual reopening caused by a step boundary;
    // natural commit/blit boundaries are not instrumentation reopenings.
    void* begin_compute(void* native_command_buffer, std::uint32_t step,
                        bool additional_encoder = false) noexcept;
    void* begin_blit(void* native_command_buffer, std::uint32_t step) noexcept;
    // Call from existing CB completion, outside stream/done locks, before
    // retiring its serial. No wait/commit/handler/dispatch is added here.
    // Root captures this Batch by shared_ptr in the existing handler. Batch
    // owns samples/device but never retains the CB, avoiding a capture cycle.
    // A gap publishes unavailable (zero) timing plus explicit summary/observer
    // metadata; it never substitutes CB or CPU durations for stage timestamps.
    Statistics complete(void* native_completed_command, std::uint64_t serial) noexcept;
    Statistics summary() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Batch(std::unique_ptr<Impl> impl);
    friend class Sampler;
};

inline Creation make_sampler(void* native_device, std::int32_t device) noexcept {
    if (!::quidra::timeline::enabled() && !::quidra::profile::enabled()) return {{}, Gap::Disabled};
    return detail::make_sampler_slow(native_device, device);
}

} // namespace quidra::timeline::metal
