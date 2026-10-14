// Direct timeline-producer fixture: synthetic intervals use the normal API.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if __has_include("../src/runtime/timeline.hpp")
#include "runtime/timeline.hpp"

namespace {
std::atomic<unsigned> source_queries{};
std::atomic<bool> reject_allocations{};
}

// A narrow heap-pressure fixture proves that buffered failure serialization
// does not need a new C++ allocation. It is never linked into the runtime.
void* operator new(std::size_t size) {
    if (reject_allocations.load(std::memory_order_relaxed)) throw std::bad_alloc{};
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace quidra::counters {
bool current_source_site(SourceSite& site) {
    source_queries.fetch_add(1, std::memory_order_relaxed);
    site = {"provider.qui", "provider-node", 7, 3};
    return true;
}
}

namespace {
using namespace quidra::timeline;

void fixture() {
    std::string file = "unit\"file\n.qui";
    std::string node = "node\\id";
    const quidra::counters::SourceSite site{file.c_str(), node.c_str(), 24, 5};
    note_host({Point::RuntimeInit, 0, 100, 20, site});
    note_wait({0, quidra::counters::WaitReason::Read, 110, 130, site});
    note_warmup({0, 7, 140, 180, 0, site});
    note_compile({0, 3, false, 180, 220, 0, site});
    note_host({Point::TimeMark, 0, 250, 30, site});
    note_host({Point::StepBoundary, 0, 300, 40, site});
    if (current_step() != 1) std::exit(3);
    note_wait(quidra::counters::WaitReason::Sync, 310, 330);
    // Completion arrives in step 1, but preserves submission's step 0.
    note_gpu({0, 11, 0, 200, 240, 260, 260, 290, 2, 64, site});
    note_gpu_step({0, 11, 0, 260, 290});
    note_host({Point::TimeMark, 1, 400, 50, site});

    DeviceCapabilities capabilities;
    capabilities.device = 0;
    capabilities.backend = "test";
    capabilities.architecture = node.c_str();
    capabilities.device_key = file.c_str();
    capabilities.compute_units = 4;
    capabilities.max_threads_per_group = 64;
    capabilities.threadgroup_memory_bytes = 32768;
    capabilities.simd_width = 8;
    capabilities.working_set_bytes = 1048576;
    capabilities.unified_memory = true;
    capabilities.unified_memory_known = true;
    capabilities.driver = "driver";
    capabilities.os_build = "os";
    note_device(capabilities);
    DeviceCapabilities unavailable;
    unavailable.device = 1;
    note_device(unavailable);
    DeviceCapabilities discrete;
    discrete.device = 2; discrete.unified_memory_known = true;
    note_device(discrete);
    GpuSampling sampling;
    sampling.device = 0; sampling.serial = 11; sampling.setup_gap = node.c_str();
    sampling.sample_buffer_allocations = 1; sampling.compute_encoders = 2;
    sampling.additional_compute_encoders = 1; sampling.sampled_passes = 3;
    sampling.calibration_queries = 2; sampling.calibration_host_ns = 40;
    sampling.resolved_intervals = 2; sampling.missing_intervals = 1;
    note_gpu_sampling(sampling);
    note_gpu_sampling_gap({0, 11, 0, file.c_str(), true, false});
    file.assign("changed");
    node.assign("changed");

    note_host({static_cast<Point>(99), 1, 1, 1, {}});
    note_wait({1, quidra::counters::WaitReason::Status, 2, 1, {}});
    note_warmup({0, 1, 2, 1});
    note_compile({0, 1, true, 2, 1});
    note_gpu({0, 20, 1, 1, 0, 0, 0, 2, 1, 1});
    note_gpu_step({0, 20, 1, 2, 1});
    // A backend may have submission metadata without available GPU timings.
    note_gpu({0, 12, 1, 410, 0, 0, 0, 0, 0, 0});
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    note_gpu({1, maximum, 1, 100, 110, 115, 120, 130,
              std::numeric_limits<std::uint32_t>::max(), maximum});

    std::vector<std::thread> callbacks;
    for (unsigned worker = 0; worker != 4; ++worker) {
        callbacks.emplace_back([worker] {
            for (unsigned i = 0; i != 25; ++i)
                note_gpu({0, 100 + worker * 25 + i, 0, 10, 11, 12, 13, 14, 1, 16});
        });
    }
    for (auto& callback : callbacks) callback.join();
}

int wait_for_parent(const char* message) {
    std::puts(message);
    std::fflush(stdout);
    return std::getchar() == '\n' ? 0 : 2;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode == "--disabled" || mode == "--compiler") {
        if (mode == "--disabled" && enabled()) return 3;
        note_point(Point::TimeMark);
        note_wait(quidra::counters::WaitReason::Sync, 1, 2);
        note_gpu({0, 1, 0, 1, 2, 3, 4, 5, 1, 1});
        note_warmup({0, 1, 1, 2});
        note_compile({0, 1, true, 1, 2});
        if (source_queries.load(std::memory_order_relaxed) != 0) return 4;
        return 0;
    }
    mark_program_started();
    if (!enabled()) return 3;
    if (mode == "--boundary-pressure") {
        const quidra::counters::SourceSite long_site{
            "a-long-generic-module-name-to-require-source-storage.qui", "node", 1, 1};
        reject_allocations.store(true, std::memory_order_relaxed);
        note_host({Point::StepBoundary, 0, 100, 20, long_site});
        reject_allocations.store(false, std::memory_order_relaxed);
        if (current_step() != 1) return 6;
        note_host({Point::TimeMark, current_step(), 200, 30, {"p.qui", "n", 2, 1}});
        note_host({Point::StepBoundary, 0, 250, 35, {}});
        if (current_step() != 1) return 7;
        note_host({Point::StepBoundary, 1, 300, 40, {"p.qui", "n", 3, 1}});
        return current_step() == 2 ? 0 : 8;
    }
    if (mode == "--pressure") {
        const quidra::counters::SourceSite long_site{
            "a-long-generic-module-name-to-require-source-storage.qui", "node", 1, 1};
        reject_allocations.store(true, std::memory_order_relaxed);
        note_host({Point::TimeMark, 0, 100, 20, long_site});
        reject_allocations.store(false, std::memory_order_relaxed);
        note_host({Point::StepBoundary, 0, 200, 30, {"p.qui", "n", 2, 1}});
        reject_allocations.store(true, std::memory_order_relaxed);
        const bool first = flush_report();
        const bool repeated = flush_report();
        reject_allocations.store(false, std::memory_order_relaxed);
        return first && repeated ? 0 : 4;
    }
    if (mode == "--real") {
        note_point(Point::ProcessStart);
        note_point(Point::RuntimeInit);
        note_point(Point::FirstStatement);
        note_point(Point::ExitBegin);
        note_point(Point::ExitEnd);
        return 0;
    }
    if (mode == "--repeat") {
        note_host({Point::StepBoundary, 0, 100, 20, {}});
        if (!flush_report()) return 4;
        const auto result = wait_for_parent("flushed");
        note_host({Point::TimeMark, 1, 999, 99, {}});
        if (!flush_report()) return 5;
        return result;
    }
    if (mode != "--records" && mode != "--failure") return 2;
    fixture();
    if (mode == "--failure") {
        if (!flush_report()) return 4;
        std::_Exit(101);
    }
    return wait_for_parent("ready");
}
#else
int main() {
    std::fputs("FAIL: runtime timeline implementation is missing\n", stderr);
    return 1;
}
#endif
