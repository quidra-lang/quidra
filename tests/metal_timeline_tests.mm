// Generic direct tests for Metal stage sampling, checked conversion and gaps.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "runtime_counters.hpp"
#include "platform/clock.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace quidra::counters {
bool current_source_site(SourceSite&) { return false; }
}

#if __has_include("../src/runtime/metal_timeline.hpp")
#include "runtime/metal_timeline.hpp"
namespace mt = quidra::timeline::metal;

@interface StageTestCounter : NSObject
@end
@implementation StageTestCounter
- (NSString*)name { return MTLCommonCounterTimestamp; }
@end
@interface StageTestCounterSet : NSObject
@end
@implementation StageTestCounterSet
- (NSString*)name { return MTLCommonCounterSetTimestamp; }
- (NSArray*)counters { return @[[[[StageTestCounter alloc] init] autorelease]]; }
@end
@interface StageTestBuffer : NSObject {
@public NSUInteger count;
@public bool error_sample, short_data;
}
@end
@implementation StageTestBuffer
- (NSData*)resolveCounterRange:(NSRange)range {
    std::vector<std::uint64_t> values(range.length);
    for (NSUInteger i = 0; i != range.length; ++i)
        values[i] = 150 + ((range.location + i) / 2) * 100 + ((range.location + i) % 2) * 50;
    if (error_sample && !values.empty()) values[0] = MTLCounterErrorValue;
    const auto bytes = values.size() * sizeof(std::uint64_t);
    return [NSData dataWithBytes:values.data() length:short_data && bytes ? bytes - 1 : bytes];
}
@end
@interface StageTestDevice : NSObject {
@public bool supported, error_sample, short_data, bad_clock;
@public unsigned queries;
}
@end
@implementation StageTestDevice
- (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)point {
    return supported && point == MTLCounterSamplingPointAtStageBoundary;
}
- (NSArray*)counterSets { return @[[[[StageTestCounterSet alloc] init] autorelease]]; }
- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor*)descriptor
                                                           error:(NSError**)error {
    (void)error;
    auto* buffer = [[StageTestBuffer alloc] init];
    buffer->count = descriptor.sampleCount;
    buffer->error_sample = error_sample;
    buffer->short_data = short_data;
    return (id<MTLCounterSampleBuffer>)buffer;
}
- (void)sampleTimestamps:(MTLTimestamp*)cpu gpuTimestamp:(MTLTimestamp*)gpu {
    *cpu = bad_clock ? 0 : quidra::platform::monotonic_ns();
    *gpu = queries++ == 0 ? 100 : 1000;
}
@end
@interface StageTestEncoder : NSObject
@end
@implementation StageTestEncoder
- (void)endEncoding {}
@end
@interface StageTestCommand : NSObject {
@public StageTestDevice* owner;
@public MTLCommandBufferStatus command_status;
@public bool refuse_sampled, refuse_ordinary;
}
- (instancetype)initWithDevice:(StageTestDevice*)device;
@end
@implementation StageTestCommand
- (instancetype)initWithDevice:(StageTestDevice*)device {
    self = [super init];
    if (self) { owner = [device retain]; command_status = MTLCommandBufferStatusScheduled; }
    return self;
}
- (void)dealloc { [owner release]; [super dealloc]; }
- (id<MTLDevice>)device { return (id<MTLDevice>)owner; }
- (MTLCommandBufferStatus)status { return command_status; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    if (refuse_ordinary) return nil;
    return (id<MTLComputeCommandEncoder>)[[[StageTestEncoder alloc] init] autorelease];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor*)descriptor {
    const auto* attachment = descriptor.sampleBufferAttachments[0];
    const auto* samples = (StageTestBuffer*)attachment.sampleBuffer;
    if (refuse_sampled || attachment.endOfEncoderSampleIndex >= samples->count) return nil;
    return [self computeCommandEncoder];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    return (id<MTLBlitCommandEncoder>)[[[StageTestEncoder alloc] init] autorelease];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor*)descriptor {
    (void)descriptor;
    return [self blitCommandEncoder];
}
@end

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Results {
    std::array<mt::StageResult, 8> values{};
    std::size_t count{};
    mt::Batch* batch{};
    std::uint64_t observed_resolved{};
};
void receive(void* context, const mt::StageResult& result) noexcept {
    auto& results = *static_cast<Results*>(context);
    if (results.batch) results.observed_resolved = results.batch->summary().resolved_intervals;
    if (results.count < results.values.size()) results.values[results.count++] = result;
}

void conversions() {
    std::uint64_t result = 0;
    expect(mt::align_timestamp({1000, 100}, {2000, 300}, 150, result) && result == 1250,
           "GPU timestamp interpolates exactly into CPU nanoseconds");
    expect(mt::align_timestamp({1000, 100}, {2000, 300}, 100, result) && result == 1000,
           "first correlation endpoint is exact");
    expect(mt::align_timestamp({1000, 100}, {2000, 300}, 300, result) && result == 2000,
           "last correlation endpoint is exact");
    expect(mt::align_timestamp({1000, 100}, {1002, 103}, 101, result) && result == 1000,
           "sub-nanosecond interpolation truncates downward");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    expect(mt::align_timestamp({1, 1}, {maximum, maximum - 1}, maximum - 2, result) && result == maximum - 2,
           "wide intermediate interpolation must not overflow");
    expect(!mt::align_timestamp({1000, 100}, {2000, 300}, 99, result), "extrapolation before baseline is refused");
    expect(!mt::align_timestamp({1000, 100}, {2000, 300}, 301, result), "extrapolation after baseline is refused");
    expect(!mt::align_timestamp({1000, 300}, {2000, 100}, 200, result), "GPU clock wrap cannot produce a duration");
    expect(!mt::align_timestamp({2000, 100}, {1000, 300}, 200, result), "CPU clock reversal is refused");
    expect(!mt::align_timestamp({1000, 100}, {2000, 100}, 100, result), "zero GPU correlation span is refused");
}

void mocks() {
    auto* device = [[StageTestDevice alloc] init];
    device->supported = true;
    auto* command = [[StageTestCommand alloc] initWithDevice:device];
    auto created = mt::make_sampler(device, 3);
    expect(created.gap == mt::Gap::None && created.sampler && created.sampler->supported(),
           "supported timestamp counter set is admitted");
    Results results;
    auto command_sampling = created.sampler->begin_command(command, 2, receive, &results);
    auto batch = command_sampling.batch;
    results.batch = batch.get();
    expect(batch && batch->sampling_active(), "supported command has stage sampling");
    expect(batch->begin_compute(command, 0) != nullptr, "first compute encoder is created");
    expect(batch->should_split_compute(), "sampled compute encoder can end at a step boundary");
    expect(batch->begin_compute(command, 1, true) != nullptr, "boundary reopening is created");
    const auto early = batch->complete(command, 123);
    expect(results.count == 0 && early.resolved_intervals == 0, "uncompleted CB is never resolved or waited on");
    // The batch keeps its device/sample resources independently of caller ownership.
    created.sampler.reset();
    [device release];
    command->command_status = MTLCommandBufferStatusCompleted;
    const auto finished = batch->complete(command, 123);
    expect(results.count == 2 && finished.resolved_intervals == 2, "completed stage timestamps are resolved");
    expect(results.observed_resolved == 2, "stage observer can inspect the final summary without deadlock");
    expect(finished.sampled_passes == 2 && finished.additional_compute_encoders == 1,
           "actual sampled and additional encoders are reported separately");
    expect(results.values[0].interval.step == 0 && results.values[1].interval.step == 1 &&
               results.values[0].interval.serial == 123,
           "completion preserves encoder step and submission serial");
    expect(results.values[0].interval.gpu_begin_ns <= results.values[0].interval.gpu_end_ns,
           "resolved stage interval is ordered in host time");
    (void)batch->complete(command, 123);
    expect(results.count == 2, "duplicate completion cannot publish twice");
    [command release];

    for (unsigned scenario = 0; scenario != 5; ++scenario) {
        auto* fallback_device = [[StageTestDevice alloc] init];
        fallback_device->supported = scenario != 0;
        fallback_device->error_sample = scenario == 2;
        fallback_device->short_data = scenario == 3;
        fallback_device->bad_clock = scenario == 4;
        auto* fallback_command = [[StageTestCommand alloc] initWithDevice:fallback_device];
        auto sampler = mt::make_sampler(fallback_device, 4).sampler;
        Results output;
        auto fallback_batch = sampler->begin_command(fallback_command, 1, receive, &output).batch;
        expect(fallback_batch->begin_compute(fallback_command, 2) != nullptr,
               "sampling failure preserves ordinary encoder creation");
        if (scenario == 1) expect(fallback_batch->begin_blit(fallback_command, 3) != nullptr,
                                  "capacity exhaustion preserves ordinary blit creation");
        fallback_command->command_status = MTLCommandBufferStatusCompleted;
        const auto status = fallback_batch->complete(fallback_command, 321);
        const mt::Gap expected[] = {mt::Gap::UnsupportedStageSampling, mt::Gap::CapacityExceeded,
            mt::Gap::CounterError, mt::Gap::MalformedResult, mt::Gap::CalibrationFailed};
        const auto index = scenario == 1 ? 1u : 0u;
        expect(output.count > index && output.values[index].gap == expected[scenario],
               "unsupported/error sample becomes an explicit gap");
        expect(output.values[index].interval.gpu_begin_ns == 0 &&
                   output.values[index].interval.gpu_end_ns == 0 && status.missing_intervals >= 1,
               "a sampling gap never fabricates valid timing");
        [fallback_command release];
        [fallback_device release];
    }

    auto* retry_device = [[StageTestDevice alloc] init];
    retry_device->supported = true;
    auto* retry_command = [[StageTestCommand alloc] initWithDevice:retry_device];
    Results retry_output;
    auto retry_sampler = mt::make_sampler(retry_device, 5).sampler;
    auto retry_batch = retry_sampler->begin_command(retry_command, 1, receive, &retry_output).batch;
    retry_command->refuse_sampled = retry_command->refuse_ordinary = true;
    expect(retry_batch->begin_compute(retry_command, 0) == nullptr,
           "encoder failure remains an encoder failure");
    retry_command->refuse_sampled = retry_command->refuse_ordinary = false;
    expect(retry_batch->begin_compute(retry_command, 1) != nullptr,
           "retry after a refused encoder can still create ordinary work");
    expect(!retry_batch->should_split_compute(), "unsampled encoder never requests a sampling-only split");
    retry_command->command_status = MTLCommandBufferStatusCompleted;
    (void)retry_batch->complete(retry_command, 456);
    expect(retry_output.count == 1 && retry_output.values[0].gap == mt::Gap::CapacityExceeded,
           "consumed counter slots are not reused after encoder refusal");
    [retry_command release];
    [retry_device release];
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 2 && std::string_view(argv[1]) == "--disabled") {
            const auto created = mt::make_sampler(nullptr, 0);
            return created.gap == mt::Gap::Disabled && !created.sampler ? 0 : 1;
        }
        conversions();
        mocks();
        if (failures) return 1;
        std::puts("Metal timeline conversion and sampling gaps: ok");
        return 0;
    }
}
#else
int main() {
    std::fputs("FAIL: Metal timeline implementation is missing\n", stderr);
    return 1;
}
#endif
