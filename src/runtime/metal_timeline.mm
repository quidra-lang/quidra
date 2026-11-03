#include "runtime/metal_timeline.hpp"
#include "platform/clock.hpp"
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#endif

namespace quidra::timeline::metal {

const char* name(Gap gap) noexcept {
    switch (gap) {
    case Gap::None: return "none";
    case Gap::Disabled: return "disabled";
    case Gap::UnsupportedOS: return "unsupported_os";
    case Gap::InvalidHandle: return "invalid_handle";
    case Gap::DeviceMismatch: return "device_mismatch";
    case Gap::UnsupportedStageSampling: return "unsupported_stage_sampling";
    case Gap::TimestampCounterMissing: return "timestamp_counter_missing";
    case Gap::AllocationFailed: return "allocation_failed";
    case Gap::CapacityExceeded: return "capacity_exceeded";
    case Gap::CalibrationFailed: return "calibration_failed";
    case Gap::EncoderSamplingFailed: return "encoder_sampling_failed";
    case Gap::CommandNotCompleted: return "command_not_completed";
    case Gap::FailedCommandBuffer: return "failed_command_buffer";
    case Gap::ResolveFailed: return "resolve_failed";
    case Gap::MalformedResult: return "malformed_result";
    case Gap::CounterError: return "counter_error";
    case Gap::UnorderedSamples: return "unordered_samples";
    case Gap::AlignmentFailed: return "alignment_failed";
    case Gap::StorageFailed: return "storage_failed";
    case Gap::Count: break;
    }
    return "unknown";
}

bool align_timestamp(ClockSample before, ClockSample after, std::uint64_t timestamp,
                     std::uint64_t& result) noexcept {
    if (after.cpu_ns <= before.cpu_ns || after.gpu_ticks <= before.gpu_ticks ||
        timestamp < before.gpu_ticks || timestamp > after.gpu_ticks) return false;
    const auto numerator = after.cpu_ns - before.cpu_ns;
    const auto denominator = after.gpu_ticks - before.gpu_ticks;
    const auto value = timestamp - before.gpu_ticks;
    std::uint64_t offset = 0;
    if (value == denominator) offset = numerator;
    else if (value <= std::numeric_limits<std::uint64_t>::max() / numerator)
        offset = value * numerator / denominator;
    else {
        // value < denominator: exact wide-product division using bounded
        // quotient/residue updates, without floating point or uint128.
        std::uint64_t residue = 0;
        for (int bit = 63; bit >= 0; --bit) {
            offset *= 2;
            if (residue >= denominator - residue) {
                residue -= denominator - residue;
                ++offset;
            } else residue *= 2;
            if (((numerator >> bit) & 1) != 0) {
                if (residue >= denominator - value) {
                    residue -= denominator - value;
                    ++offset;
                } else residue += value;
            }
        }
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - before.cpu_ns) return false;
    result = before.cpu_ns + offset;
    return true;
}

struct Sampler::Impl {
    std::int32_t index{};
    Gap gap{Gap::UnsupportedOS};
#ifdef __APPLE__
    id<MTLDevice> device{nil};
    id<MTLCounterSet> timestamps{nil};
    ~Impl() { [timestamps release]; [device release]; }
#endif
};
struct Batch::Impl {
    struct Pass {
        std::uint32_t step{};
        std::size_t sample{};
        Gap gap{Gap::None};
        bool compute{}, additional{};
        Gap resolved_gap{Gap::None};
        std::uint64_t begin{}, end{};
        std::uint64_t index{};
    };
    std::shared_ptr<Sampler::Impl> sampler;
    void* command{}; // borrowed identity only, never retained/dereferenced after completion
    mutable std::mutex mutex;
    bool active{}, completed{}, compute_sampled{};
    std::size_t capacity{}, used_samples{};
    StageObserver observer{};
    void* context{};
    Statistics stats;
    std::vector<Pass> passes;
#ifdef __APPLE__
    id<MTLCounterSampleBuffer> samples{nil};
    ~Impl() { [samples release]; }

    bool correlate(ClockSample& sample) {
        if (@available(macOS 10.15, *)) {
            const auto before = platform::monotonic_ns();
            MTLTimestamp cpu = 0, gpu = 0;
            [sampler->device sampleTimestamps:&cpu gpuTimestamp:&gpu];
            const auto after = platform::monotonic_ns();
            ++stats.calibration_queries;
            if (after >= before && after - before <= std::numeric_limits<std::uint64_t>::max() - stats.calibration_host_ns)
                stats.calibration_host_ns += after - before;
            if (before == 0 || after < before || cpu < before || cpu > after || gpu == 0 ||
                gpu == MTLCounterErrorValue) return false;
            sample = {cpu, gpu};
            return true;
        }
        return false;
    }

    void* begin(void* native_command, std::uint32_t step, bool compute, bool additional) {
        if (compute) compute_sampled = false;
        if (completed || native_command != command || !native_command) return nullptr;
        id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)native_command;
        Gap gap = stats.setup_gap;
        bool sampled = false;
        std::size_t first = used_samples;
        id encoder = nil;
        if (active && used_samples / 2 < capacity) {
            if (@available(macOS 11.0, *)) {
                used_samples += 2;
                if (compute) {
                    auto* descriptor = [MTLComputePassDescriptor computePassDescriptor];
                    descriptor.dispatchType = MTLDispatchTypeSerial;
                    auto* attachment = descriptor.sampleBufferAttachments[0];
                    attachment.sampleBuffer = samples;
                    attachment.startOfEncoderSampleIndex = first;
                    attachment.endOfEncoderSampleIndex = first + 1;
                    encoder = [cb computeCommandEncoderWithDescriptor:descriptor];
                } else {
                    auto* descriptor = [MTLBlitPassDescriptor blitPassDescriptor];
                    auto* attachment = descriptor.sampleBufferAttachments[0];
                    attachment.sampleBuffer = samples;
                    attachment.startOfEncoderSampleIndex = first;
                    attachment.endOfEncoderSampleIndex = first + 1;
                    encoder = [cb blitCommandEncoderWithDescriptor:descriptor];
                }
                sampled = encoder != nil;
                if (!sampled) gap = Gap::EncoderSamplingFailed;
            }
        } else if (active) gap = Gap::CapacityExceeded;
        if (!encoder) encoder = compute ? (id)[cb computeCommandEncoder] : (id)[cb blitCommandEncoder];
        if (!encoder) {
            ++stats.gaps[static_cast<std::size_t>(Gap::EncoderSamplingFailed)];
            return nullptr;
        }
        if (compute) {
            ++stats.compute_encoders;
            compute_sampled = sampled;
            if (additional) ++stats.additional_compute_encoders;
        } else ++stats.blit_encoders;
        if (sampled) ++stats.sampled_passes;
        try { passes.push_back({step, first, sampled ? Gap::None : gap, compute, additional,
                                Gap::None,0,0,stats.compute_encoders+stats.blit_encoders-1}); }
        catch (...) { ++stats.gaps[static_cast<std::size_t>(Gap::StorageFailed)]; }
        return (void*)encoder;
    }

    void resolve(Pass& pass, Gap gap, std::uint64_t begin, std::uint64_t end) {
        pass.resolved_gap = gap;
        pass.begin = begin;
        pass.end = end;
        if (gap == Gap::None) ++stats.resolved_intervals;
        else {
            ++stats.missing_intervals;
            ++stats.gaps[static_cast<std::size_t>(gap)];
        }
    }
    void publish(const Pass& pass, std::uint64_t serial) {
        StageResult result{{sampler->index, serial, pass.step, pass.begin, pass.end},
            pass.resolved_gap, pass.compute, pass.additional,pass.index};
        ::quidra::timeline::note_gpu_step(result.interval);
        if (observer) observer(context, result);
    }
#endif
};

Sampler::Sampler(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Sampler::~Sampler() = default;
bool Sampler::supported() const noexcept { return impl_->gap == Gap::None; }
Gap Sampler::setup_gap() const noexcept { return impl_->gap; }
Batch::Batch(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Batch::~Batch() = default;
bool Batch::sampling_active() const noexcept { return impl_->active; }
bool Batch::should_split_compute() const noexcept {
    try { std::lock_guard lock(impl_->mutex); return !impl_->completed && impl_->compute_sampled; }
    catch (...) { return false; }
}

namespace detail {
Creation make_sampler_slow(void* native_device, std::int32_t index) noexcept {
    try {
        if (!native_device) return {{}, Gap::InvalidHandle};
        auto impl = std::make_shared<Sampler::Impl>();
        impl->index = index;
#ifdef __APPLE__
        impl->device = [(id<MTLDevice>)native_device retain];
        if (@available(macOS 11.0, *)) {
            if (![impl->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
                impl->gap = Gap::UnsupportedStageSampling;
            else {
                impl->gap = Gap::TimestampCounterMissing;
                for (id<MTLCounterSet> counter_set in [impl->device counterSets]) {
                    if (![[counter_set name] isEqualToString:MTLCommonCounterSetTimestamp]) continue;
                    for (id<MTLCounter> counter in [counter_set counters]) {
                        if ([[counter name] isEqualToString:MTLCommonCounterTimestamp]) {
                            impl->timestamps = [counter_set retain];
                            impl->gap = Gap::None;
                            break;
                        }
                    }
                    if (impl->timestamps) break;
                }
            }
        }
#endif
        const auto gap = impl->gap;
        return {std::shared_ptr<Sampler>(new Sampler(std::move(impl))), gap};
    } catch (...) { return {{}, Gap::AllocationFailed}; }
}
}

CommandCreation Sampler::begin_command(void* native_command, std::size_t max_passes,
                                      StageObserver observer, void* context) const noexcept {
    try {
        if (!native_command) return {{}, Gap::InvalidHandle};
        auto state = std::make_unique<Batch::Impl>();
        state->sampler = impl_;
        state->command = native_command;
        state->capacity = max_passes;
        state->observer = observer;
        state->context = context;
        state->stats.setup_gap = impl_->gap;
        // Two 8-byte samples per pass; keep the requested sample buffer within
        // the 32KiB Metal family limit. Failed allocation is an explicit gap.
        if (max_passes == 0 || max_passes > 2048) state->stats.setup_gap = Gap::CapacityExceeded;
        state->passes.reserve(max_passes <= 2048 ? max_passes : 0);
#ifdef __APPLE__
        id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)native_command;
        if ([cb device] != impl_->device) state->stats.setup_gap = Gap::DeviceMismatch;
        if (state->stats.setup_gap == Gap::None) {
            if (@available(macOS 11.0, *)) {
                auto* descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
                descriptor.counterSet = impl_->timestamps;
                descriptor.storageMode = MTLStorageModeShared;
                descriptor.sampleCount = max_passes * 2;
                NSError* error = nil;
                state->samples = [impl_->device newCounterSampleBufferWithDescriptor:descriptor error:&error];
                [descriptor release];
                if (!state->samples) state->stats.setup_gap = Gap::AllocationFailed;
                else {
                    ++state->stats.sample_buffer_allocations;
                    if (!state->correlate(state->stats.before)) state->stats.setup_gap = Gap::CalibrationFailed;
                }
            }
        }
#endif
        state->active = state->stats.setup_gap == Gap::None;
        const auto gap = state->stats.setup_gap;
        return {std::shared_ptr<Batch>(new Batch(std::move(state))), gap};
    } catch (...) { return {{}, Gap::AllocationFailed}; }
}

void* Batch::begin_compute(void* native_command, std::uint32_t step, bool additional) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
#ifdef __APPLE__
        return impl_->begin(native_command, step, true, additional);
#else
        (void)native_command; (void)step; (void)additional;
        return nullptr;
#endif
    } catch (...) { return nullptr; }
}
void* Batch::begin_blit(void* native_command, std::uint32_t step) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
#ifdef __APPLE__
        return impl_->begin(native_command, step, false, false);
#else
        (void)native_command; (void)step;
        return nullptr;
#endif
    } catch (...) { return nullptr; }
}

Statistics Batch::summary() const noexcept {
    try { std::lock_guard lock(impl_->mutex); return impl_->stats; }
    catch (...) { Statistics failed; failed.setup_gap = Gap::StorageFailed; return failed; }
}
Statistics Batch::complete(void* native_completed, std::uint64_t serial) noexcept {
    try {
        std::unique_lock lock(impl_->mutex);
        if (impl_->completed) return impl_->stats;
#ifdef __APPLE__
        if (native_completed != impl_->command || !native_completed) {
            ++impl_->stats.gaps[static_cast<std::size_t>(Gap::InvalidHandle)];
            return impl_->stats;
        }
        id<MTLCommandBuffer> command = (id<MTLCommandBuffer>)native_completed;
        const auto status = [command status];
        if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) {
            ++impl_->stats.gaps[static_cast<std::size_t>(Gap::CommandNotCompleted)];
            return impl_->stats;
        }
        impl_->completed = true;
        @autoreleasepool {
            Gap failure = status == MTLCommandBufferStatusError ? Gap::FailedCommandBuffer : Gap::None;
            NSData* data = nil;
            if (failure == Gap::None && impl_->stats.sampled_passes != 0) {
                if (!impl_->correlate(impl_->stats.after) ||
                    impl_->stats.after.cpu_ns <= impl_->stats.before.cpu_ns ||
                    impl_->stats.after.gpu_ticks <= impl_->stats.before.gpu_ticks) failure = Gap::CalibrationFailed;
                else {
                    if (@available(macOS 10.15, *))
                        data = [impl_->samples resolveCounterRange:NSMakeRange(0, impl_->used_samples)];
                    if (!data) failure = Gap::ResolveFailed;
                    else if ([data length] != impl_->used_samples * sizeof(std::uint64_t) || ![data bytes])
                        failure = Gap::MalformedResult;
                }
            }
            for (auto& pass : impl_->passes) {
                Gap gap = failure != Gap::None ? failure : pass.gap;
                std::uint64_t begin = 0, end = 0;
                if (gap == Gap::None) {
                    const auto* bytes = static_cast<const unsigned char*>([data bytes]);
                    std::uint64_t raw_begin = 0, raw_end = 0;
                    std::memcpy(&raw_begin, bytes + pass.sample * sizeof(raw_begin), sizeof(raw_begin));
                    std::memcpy(&raw_end, bytes + (pass.sample + 1) * sizeof(raw_end), sizeof(raw_end));
                    if (raw_begin == 0 || raw_end == 0 || raw_begin == MTLCounterErrorValue || raw_end == MTLCounterErrorValue)
                        gap = Gap::CounterError;
                    else if (raw_end < raw_begin) gap = Gap::UnorderedSamples;
                    else if (!align_timestamp(impl_->stats.before, impl_->stats.after, raw_begin, begin) ||
                             !align_timestamp(impl_->stats.before, impl_->stats.after, raw_end, end) || begin == 0)
                        gap = Gap::AlignmentFailed;
                }
                if (gap != Gap::None) begin = end = 0;
                impl_->resolve(pass, gap, begin, end);
            }
        }
        const auto result = impl_->stats;
        // Results are now immutable. Never hold the batch lock while invoking
        // an observer; it may inspect summary() or other observation state.
        lock.unlock();
        for (const auto& pass : impl_->passes) impl_->publish(pass, serial);
        return result;
#else
        (void)native_completed; (void)serial;
        impl_->completed = true;
#endif
        return impl_->stats;
    } catch (...) { Statistics failed; failed.setup_gap = Gap::StorageFailed; return failed; }
}

} // namespace quidra::timeline::metal
