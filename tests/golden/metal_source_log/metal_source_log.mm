// Test-only Metal source logger for gate Gobj (tests/golden/README.md).
//
// Injected into a program with DYLD_INSERT_LIBRARIES, it records every source
// string the program passes to -[MTLDevice newLibraryWithSource:options:error:]
// (and the completion-handler variant) in the file named by
// QUIDRA_METAL_SOURCE_LOG, one record per call:
//   === <byte count>\n<source>\n
// Runtime-generated kernel text (Core's device_*.inc, package natives) is not
// in any object file, so Gobj compares these logs as multisets between the
// base and the head build (compare_logs.py).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "platform/environment.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

namespace {

using NewLibrary = id (*)(id, SEL, NSString*, MTLCompileOptions*, NSError**);
using NewLibraryAsync = void (*)(id, SEL, NSString*, MTLCompileOptions*, id);

NewLibrary original_new_library = nullptr;
NewLibraryAsync original_new_library_async = nullptr;
std::mutex log_mutex;

void record(NSString* source) {
    const std::optional<std::string> path =
        quidra::platform::environment_value("QUIDRA_METAL_SOURCE_LOG");
    if (!path || !source) return;
    const char* text = [source UTF8String];
    if (!text) return;
    const std::lock_guard<std::mutex> lock(log_mutex);
    if (FILE* file = std::fopen(path->c_str(), "ab")) {
        const auto length = std::strlen(text);
        std::fprintf(file, "=== %zu\n", length);
        std::fwrite(text, 1, length, file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

id logged_new_library(id self, SEL command, NSString* source, MTLCompileOptions* options,
                      NSError** error) {
    record(source);
    return original_new_library(self, command, source, options, error);
}

void logged_new_library_async(id self, SEL command, NSString* source,
                              MTLCompileOptions* options, id handler) {
    record(source);
    original_new_library_async(self, command, source, options, handler);
}

__attribute__((constructor)) void install_metal_source_log() {
    if (!quidra::platform::environment_has("QUIDRA_METAL_SOURCE_LOG")) return;
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        for (id<MTLDevice> device in devices) {
            Class type = [(id)device class];
            if (Method method = class_getInstanceMethod(
                    type, @selector(newLibraryWithSource:options:error:))) {
                IMP previous = method_setImplementation(method, (IMP)logged_new_library);
                if (previous != (IMP)logged_new_library) {
                    original_new_library = reinterpret_cast<NewLibrary>(previous);
                }
            }
            if (Method method = class_getInstanceMethod(
                    type, @selector(newLibraryWithSource:options:completionHandler:))) {
                IMP previous = method_setImplementation(method, (IMP)logged_new_library_async);
                if (previous != (IMP)logged_new_library_async) {
                    original_new_library_async = reinterpret_cast<NewLibraryAsync>(previous);
                }
            }
        }
    }
}

} // namespace
