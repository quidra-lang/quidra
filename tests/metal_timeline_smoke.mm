// One-command-buffer hardware correctness smoke, not a benchmark.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "runtime/metal_timeline.hpp"
#include <cstdio>
#include <dispatch/dispatch.h>

namespace quidra::counters {
bool current_source_site(SourceSite&) { return false; }
}

int main() {
    @autoreleasepool {
        if (!quidra::timeline::enabled()) return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("Metal stage sampling: no device"); return 77; }
        auto created = quidra::timeline::metal::make_sampler((void*)device, 0);
        if (!created.sampler || !created.sampler->supported()) {
            std::printf("Metal stage sampling unavailable: %s\n", quidra::timeline::metal::name(created.gap));
            [device release];
            return 77;
        }
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void stage_write(device uint* values [[buffer(0)]], constant uint& index [[buffer(1)]]) { values[index] = index + 1; }"
            options:nil error:&error];
        if (!library) { std::fputs("Metal smoke library creation failed\n", stderr); [device release]; return 1; }
        id<MTLFunction> function = [library newFunctionWithName:@"stage_write"];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        [library release];
        if (!pipeline) { std::fputs("Metal smoke pipeline creation failed\n", stderr); [device release]; return 1; }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [[queue commandBuffer] retain];
        id<MTLBuffer> values = [device newBufferWithLength:2 * sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
        quidra::timeline::mark_program_started();
        auto sampling = created.sampler->begin_command((void*)command, 3);
        if (!sampling.batch || !sampling.batch->sampling_active()) {
            std::printf("Metal command sampling unavailable: %s\n", quidra::timeline::metal::name(sampling.gap));
            [values release]; [command release]; [queue release]; [pipeline release]; [device release];
            return 1;
        }
        auto batch = sampling.batch;
        @autoreleasepool {
            id<MTLBlitCommandEncoder> clear = (id<MTLBlitCommandEncoder>)batch->begin_blit((void*)command, 0);
            if (!clear) return 1;
            [clear fillBuffer:values range:NSMakeRange(0, 2 * sizeof(std::uint32_t)) value:0];
            [clear endEncoding];
            for (std::uint32_t index = 0; index != 2; ++index) {
                id<MTLComputeCommandEncoder> encoder =
                    (id<MTLComputeCommandEncoder>)batch->begin_compute((void*)command, index, index == 1);
                if (!encoder || !batch->should_split_compute()) return 1;
                [encoder setComputePipelineState:pipeline];
                [encoder setBuffer:values offset:0 atIndex:0];
                [encoder setBytes:&index length:sizeof(index) atIndex:1];
                [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encoder endEncoding];
            }
        }
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        __block quidra::timeline::metal::Statistics result;
        [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            result = batch->complete((void*)finished, 1);
            dispatch_semaphore_signal(completed);
        }];
        // Completion's batch ownership survives release of the setup sampler.
        created.sampler.reset();
        [command commit];
        // The smoke waits for its own test CB. The production helper never waits.
        const auto timeout = dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        bool okay = timeout == 0 && [command status] == MTLCommandBufferStatusCompleted;
        if (okay) {
            const auto* data = static_cast<const std::uint32_t*>([values contents]);
            okay = data[0] == 1 && data[1] == 2 && result.resolved_intervals == 3 &&
                result.missing_intervals == 0 && result.sampled_passes == 3 &&
                result.additional_compute_encoders == 1 && result.calibration_queries == 2;
        }
        if (timeout == 0) {
            [values release]; [command release]; [queue release]; [pipeline release]; [device release];
            dispatch_release(completed);
        }
        if (!okay) {
            std::fprintf(stderr, "Metal stage sampling smoke failed: setup=%s, resolved=%llu, missing=%llu\n",
                quidra::timeline::metal::name(result.setup_gap),
                static_cast<unsigned long long>(result.resolved_intervals),
                static_cast<unsigned long long>(result.missing_intervals));
            return 1;
        }
        std::puts("Metal stage sampling hardware smoke: ok");
        return 0;
    }
}
