#include "runtime/metal_capture.hpp"
#include "platform/clock.hpp"
#include <algorithm>
#include <limits>
#include <new>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace quidra::profile::metal::detail {
namespace {
struct NativeContext {
    Status* status{};
    MTLCaptureManager* manager{nil};
    id retained_device{nil};
    bool query_known{true};
};
void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}
void text_error(std::string&, NSString*, NativeError&) noexcept;
bool available(void*) noexcept { if (@available(macOS 10.15, *)) return true; return false; }
bool acquire(void* p) noexcept {
    @try {
        if (@available(macOS 10.15, *)) {
            auto& self = *static_cast<NativeContext*>(p);
            self.manager = [[MTLCaptureManager sharedCaptureManager] retain];
            return self.manager != nil;
        }
    } @catch (NSException*) {}
    return false;
}
bool supported(void* p) noexcept {
    auto& self = *static_cast<NativeContext*>(p); self.query_known = true;
    @try { if (@available(macOS 10.15, *)) return [self.manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]; }
    @catch (NSException* exception) {
        self.query_known = false; text_error(self.status->error.domain, @"NSException", self.status->error);
        text_error(self.status->error.message, exception.reason, self.status->error);
    }
    return false;
}
bool capturing(void* p) noexcept {
    auto& self = *static_cast<NativeContext*>(p); self.query_known = true;
    @try { return [self.manager isCapturing]; }
    @catch (NSException* exception) {
        self.query_known = false; text_error(self.status->error.domain, @"NSException", self.status->error);
        text_error(self.status->error.message, exception.reason, self.status->error);
    }
    return false;
}
bool query_succeeded(void* p) noexcept { return static_cast<NativeContext*>(p)->query_known; }
void text_error(std::string& out, NSString* text, NativeError& error) noexcept {
    @try {
        const char* utf8 = [text UTF8String];
        if (!utf8) return;
        constexpr std::size_t limit = 512;
        const auto size = std::char_traits<char>::length(utf8);
        if (size > limit) error.truncated = true;
        auto cut = std::min(size, limit);
        if (cut < size) while (cut != 0 && (static_cast<unsigned char>(utf8[cut]) & 0xc0) == 0x80) --cut;
        try { out.assign(utf8, cut); }
        catch (...) { error.storage_failed = true; }
    } @catch (NSException*) { error.storage_failed = true; }
}
bool start(void* p, void* device, const char* path, NativeError& error) noexcept {
    MTLCaptureDescriptor* descriptor = nil;
    BOOL result = NO;
    @try {
        if (@available(macOS 10.15, *)) {
            auto& self = *static_cast<NativeContext*>(p);
            descriptor = [[MTLCaptureDescriptor alloc] init];
            if (descriptor) increment(self.status->descriptors_allocated);
            descriptor.captureObject = (id<MTLDevice>)device;
            descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
            NSString* file = [NSString stringWithUTF8String:path];
            descriptor.outputURL = file ? [NSURL fileURLWithPath:file isDirectory:NO] : nil;
            NSError* native_error = nil;
            if (descriptor.outputURL) result = [self.manager startCaptureWithDescriptor:descriptor error:&native_error];
            if (result) {
                self.retained_device = [(id)device retain];
                if (self.retained_device) increment(self.status->native_device_retains);
            }
            if (!result && native_error) {
                error.code = static_cast<std::int64_t>(native_error.code);
                text_error(error.domain, native_error.domain, error);
                text_error(error.message, native_error.localizedDescription, error);
            }
        }
    } @catch (NSException* exception) {
        text_error(error.domain, @"NSException", error);
        text_error(error.message, exception.reason, error);
    } @finally { [descriptor release]; }
    return result;
}
bool stop(void* p) noexcept {
    @try { [static_cast<NativeContext*>(p)->manager stopCapture]; return true; } @catch (NSException*) {}
    return false;
}
void release(void* p) noexcept {
    @try {
        auto& self = *static_cast<NativeContext*>(p);
        [self.manager release]; self.manager = nil;
        [self.retained_device release]; self.retained_device = nil;
    } @catch (NSException*) {}
}
void destroy(void* p) noexcept { release(p); delete static_cast<NativeContext*>(p); }
std::uint64_t clock(void*) noexcept {
    try { return platform::monotonic_ns(); } catch (...) { return 0; }
}
}
NativeApi make_native_api(Status& status) noexcept {
    auto* context = new (std::nothrow) NativeContext;
    if (!context) return {};
    context->status = &status;
    return {context, available, acquire, supported, capturing, start, stop, release, clock, destroy, query_succeeded};
}
} // namespace quidra::profile::metal::detail
