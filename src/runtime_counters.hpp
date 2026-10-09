#pragma once

// Runtime counters. Every count observes the device runtime; none of
// them changes what the runtime does. Counting is off unless QUIDRA_COUNTERS
// names a report file (or a test enables it), and the disabled path of every
// hook is one relaxed atomic load.

#include "quidra/abi/runtime_failure.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::counters {

enum class Id : unsigned {
    // Backend-neutral device work requested by Core. The fake test backend
    // produces the same values as Metal for the same Core program.
    CoreDispatches,        // Core compute kernels encoded or launched
    Allocations,           // device::allocate calls (Core and package scratch)
    AllocationBytes,
    Uploads,               // host -> device copies
    UploadBytes,
    Readbacks,             // device -> host copies
    ReadbackBytes,
    Fills,                 // zero fills of device ranges
    FillBytes,
    DeviceCopies,          // same-device copies
    DeviceCopyBytes,
    ValidationSlots,       // device status words acquired for checked ops
    Synchronizations,      // device::synchronize calls that reached a backend
    // Metal submission shape.
    MetalCommandBuffers,   // command buffers committed by Core
    MetalMarkerBuffers,    // empty command buffers committed only to wait
    MetalComputeEncoders,
    MetalBlitEncoders,
    MetalNewBuffers,       // newBufferWithLength/newBufferWithBytes calls
    MetalPoolHits,         // allocations served from the buffer pool
    MetalDirectUploads,    // uploads written through shared storage, no command
    MetalStagedUploads,    // uploads that needed an ordered device copy
    MetalDirectFills,      // fills written through shared storage, no command
    MetalKernelFills,      // fills encoded as compute zero kernels
    MetalPipelineCompiles, // Core pipeline compilations
    // Host waits on GPU completion (every wait, then split by reason).
    HostWaits,
    HostWaitsSync,         // device synchronization (gpu.sync, time sync, exit, casts)
    HostWaitsRead,         // host read of device bytes
    HostWaitsStatus,       // synchronous status-slot wait requested by a package
    // Package interaction with Core's device stream.
    PackageQueueBorrows,   // native queue handles lent to packages
    PackageBorrowFlushes,  // open command buffers committed for a package borrow
    PackageEncodeHolds,    // package holds of Core's stream (qcore_device_encode_begin)
    PackageEncodeScopes,   // package encodes into Core's command buffer/encoder
    PackageDeferredChecks, // package status words deferred to the next sync
    PackageRefusals,       // package fast-path refusals reported by packages
    PackageCompletionWaits, // synchronizations that waited for completion callbacks
    PackageScopeViolations, // package encode protocol violations (refusals, encodes left open)
    PackageWarmups,        // package warm-up calls finished (one per function and device)
    Count
};

const char* name(Id id);

namespace detail {
extern std::atomic<bool> enabled;
void add_slow(Id id, std::uint64_t amount);
}

inline bool enabled() {
    return detail::enabled.load(std::memory_order_relaxed);
}

inline void add(Id id, std::uint64_t amount = 1) {
    if (!enabled()) return;
    detail::add_slow(id, amount);
}

// Records one host wait, attributed to the source site that caused it when
// the runtime knows one.
enum class WaitReason { Sync, Read, Status };
void note_wait(WaitReason reason);

// A package reported that its fast path refused an input (status code).
void note_refusal(const char* owner, const char* function, int status);

// Program-step delimiters for the QUIDRA_COUNTERS report.
enum class StepEvent { Backward, Mark };
void step_boundary(StepEvent event, const char* label = nullptr);

// Test hooks: totals since the process started (or since reset()).
void set_enabled(bool value);
std::uint64_t value(Id id);
std::vector<std::uint64_t> snapshot();
void reset();

// Source attribution. The runtime registers a provider that reports the
// statement currently executing; the device layer (also linked into the
// compiler CLI, which has no provider) only asks.
struct SourceSite {
    const char* file{};
    const char* node_id{};
    unsigned long long line{};
    unsigned long long column{};
};
using SourceSiteProvider = bool (*)(SourceSite& site);
void set_source_site_provider(SourceSiteProvider provider);
bool current_source_site(SourceSite& site);
// The innermost statement of user code (a package's statements excluded):
// the origin a deferred GPU check names.
void set_user_source_site_provider(SourceSiteProvider provider);
bool current_user_source_site(SourceSite& site);

// Failures the device layer raises without a site of its own (CPU_THREADS,
// PARALLEL_BODY, GPU_ASYNC at exit, GPU_SCOPE in a completion callback). The
// runtime registers its reporter, which reports them at the user's statement
// (or names the root file when none runs) and ends the program; with
// `immediate_exit` the process ends at once after the report (flush, _Exit),
// for the callers where exit handlers must not run. Without a reporter (the
// compiler, which also compiles the device layer) the failure is printed on
// one line and the program ends the same way.
using UnlocatedFailureReporter = void (*)(abi::FailureReason reason, std::string_view message,
                                          bool immediate_exit);
void set_unlocated_failure_reporter(UnlocatedFailureReporter reporter);
[[noreturn]] void report_unlocated_failure(abi::FailureReason reason, std::string_view message,
                                           bool immediate_exit);
std::string format_source_site(const SourceSite& site);

} // namespace quidra::counters
