// Direct native capture correctness smoke; never a performance measurement.
#include "runtime/metal_capture.hpp"
#include "platform/environment.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace mc = quidra::profile::metal;
int main(int argc, char** argv) {
    if (!quidra::platform::environment_value_is("MTL_CAPTURE_ENABLED", "1")) {
        std::puts("SKIP: run only in a child with MTL_CAPTURE_ENABLED=1"); return 77;
    }
    if (argc != 2) { std::fprintf(stderr, "expected fresh absolute .gputrace destination\n"); return 2; }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP: no Metal device"); return 77; }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> value = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
        if (!queue || !value) { [value release]; [queue release]; [device release]; return 2; }
        id<MTLCommandBuffer> prior = [queue commandBuffer];
        prior.label = @"capture.prior";
        id<MTLBlitCommandEncoder> prior_encoder = [prior blitCommandEncoder];
        [prior_encoder fillBuffer:value range:NSMakeRange(0, 4) value:7];
        [prior_encoder endEncoding]; [prior commit]; [prior waitUntilCompleted];
        std::array<unsigned char, 4> baseline{};
        if (value.contents) std::memcpy(baseline.data(), value.contents, baseline.size());
        auto created = mc::make_capture({true, 3, argv[1], {}});
        if (!created.capture) {
            std::fprintf(stderr, "capture preparation unavailable: %s\n", mc::name(created.status.state));
            [value release]; [queue release]; [device release]; return 1;
        }
        auto& capture = *created.capture;
        const auto token = capture.before_command({3, 2, 0, device, {}});
        if (!token) {
            const auto status = capture.snapshot();
            const bool unsupported=status.state==mc::State::Unsupported || status.state==mc::State::NoMetalWork ||
                status.gaps[static_cast<std::size_t>(mc::Gap::AlreadyCapturing)]!=0;
            std::fprintf(stderr, "%s: capture %s, domain=%s code=%lld message=%s\n", unsupported ? "SKIP" : "FAIL", mc::name(status.state),
                         status.error.domain.c_str(), static_cast<long long>(status.error.code), status.error.message.c_str());
            [value release]; [queue release]; [device release]; return unsupported ? 77 : 1;
        }
        id<MTLCommandBuffer> target = [queue commandBuffer];
        target.label = @"capture.target";
        if (!target || !capture.command_created(token)) {
            capture.stop(mc::Reason::ImmediateFailure, {}, true);
            [value release]; [queue release]; [device release]; return 1;
        }
        id<MTLBlitCommandEncoder> encoder = [target blitCommandEncoder];
        [encoder fillBuffer:value range:NSMakeRange(0, 4) value:7]; [encoder endEncoding];
        [target commit];
        const bool noted = capture.command_committed(token, 3, 3);
        capture.boundary(3); // Stop follows actual commit; the helper adds no wait.
        const auto status = capture.snapshot();
        [target waitUntilCompleted]; // Only this smoke checks numerical completion.
        const auto* bytes = static_cast<const unsigned char*>(value.contents);
        const bool correct = bytes && bytes[0] == 7 && bytes[1] == 7 && bytes[2] == 7 && bytes[3] == 7 &&
            std::memcmp(bytes, baseline.data(), baseline.size()) == 0;
        const bool success = noted && correct && target.status == MTLCommandBufferStatusCompleted &&
            status.stopped && status.exact_core_step && status.artifact_exists_known && status.artifact_exists;
        std::printf("Metal capture native: state=%s started=%d stopped=%d exact_core=%d artifact_known=%d artifact_exists=%d values=%d\n",
                    mc::name(status.state), status.start_succeeded, status.stopped, status.exact_core_step,
                    status.artifact_exists_known, status.artifact_exists, correct);
        std::printf("manager_queries=%llu starts=%llu stops=%llu created=%llu committed=%llu extra_commits=%llu\n",
                    static_cast<unsigned long long>(status.manager_queries), static_cast<unsigned long long>(status.start_attempts),
                    static_cast<unsigned long long>(status.stop_attempts), static_cast<unsigned long long>(status.created_commands),
                    static_cast<unsigned long long>(status.committed_commands), static_cast<unsigned long long>(status.extra_commits));
        [value release]; [queue release]; [device release];
        return success ? 0 : 1;
    }
}
