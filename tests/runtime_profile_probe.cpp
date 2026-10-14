#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if __has_include("../src/runtime/profile.hpp")
#include "runtime/profile.hpp"
namespace { std::atomic<bool> reject{}; std::atomic<unsigned> queries{}; }
void* operator new(std::size_t n) {
    if (reject.load(std::memory_order_relaxed)) throw std::bad_alloc{};
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace quidra::counters {
bool current_source_site(SourceSite& site) {
    ++queries; site = {"provider.qui", "provider-node", 2, 3}; return true;
}
}
using namespace quidra::profile;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode == "--off" || mode == "--compiler") {
        statement_changed(nullptr); record({});
        range_end(range_begin("test", "range"));
        return queries == 0 ? 0 : 3;
    }
    mark_program_started();
    if (!enabled()) return 3;
    if (mode == "--gpu-roles") {
        ProfileEvent event; event.site = {"roles.qui", "roles", 1, 1};
        event.owner = "test"; event.device = 0;
        event.gpu_begin_ns = 100; event.gpu_end_ns = 200; event.bytes = 7;
        event.kind = ProfileEvent::Kind::CommandBuffer; event.name = event.kernel = "explicit activity";
        event.gpu_role = ProfileEvent::GpuRole::Activity; record(event);
        event.kind = ProfileEvent::Kind::Dispatch; event.name = event.kernel = "explicit container";
        event.gpu_role = ProfileEvent::GpuRole::Container; record(event);
        event.gpu_role = ProfileEvent::GpuRole::None; record(event);
        event.device = -1; event.gpu_begin_ns = event.gpu_end_ns = 0;
        event.gpu_role = ProfileEvent::GpuRole::Activity; record(event);
        event.gpu_role = static_cast<ProfileEvent::GpuRole>(255); record(event);
        event.gpu_role = ProfileEvent::GpuRole::Automatic;
        event.name = event.kernel = "CPU dispatch"; record(event);
        return flush_report() ? 0 : 4;
    }
    if (mode == "--typed-gpu") {
        ProfileEvent event; event.site = {"mixed.qui", "mixed", 1, 1};
        event.owner = "core"; event.name = event.kernel = "shared";
        event.device = 0; event.host_begin_ns = 1000; event.host_end_ns = 1100;
        event.kind = ProfileEvent::Kind::Dispatch;
        event.gpu_begin_ns = 2000; event.gpu_end_ns = 3000; event.bytes = 16; record(event);
        event.kind = ProfileEvent::Kind::Copy;
        event.gpu_begin_ns = 3000; event.gpu_end_ns = 5000; event.bytes = 32; record(event);
        event.kind = ProfileEvent::Kind::Dispatch;
        event.gpu_begin_ns = event.gpu_end_ns = 0; event.bytes = 8; record(event);
        event.kind = ProfileEvent::Kind::CommandBuffer;
        event.gpu_begin_ns = 1000; event.gpu_end_ns = 20000;
        event.bytes = std::numeric_limits<std::uint64_t>::max(); record(event);
        event.kind = ProfileEvent::Kind::PlanRun;
        event.gpu_end_ns = 10000; event.bytes = 100; record(event);
        event.kind = ProfileEvent::Kind::Replay;
        event.gpu_begin_ns = event.gpu_end_ns = 0; event.bytes = 200; record(event);
        event.kind = ProfileEvent::Kind::HostOp; event.device = -1; event.bytes = 4; record(event);
        event.kind = ProfileEvent::Kind::CommandBuffer; event.device = 0;
        event.site = {"container.qui", "container", 2, 1};
        event.name = event.kernel = "large-container";
        event.gpu_begin_ns = 1000; event.gpu_end_ns = 1000000; event.bytes = 1; record(event);
        reject = true;
        const bool okay = flush_report() && flush_report();
        reject = false;
        return okay ? 0 : 4;
    }
    if (mode == "--transition-pressure") {
        const quidra::counters::SourceSite first{"first.qui", "first", 1, 1};
        const quidra::counters::SourceSite dropped{
            "long-source-name-to-require-a-new-string-allocation.qui", "dropped", 2, 1};
        const quidra::counters::SourceSite last{"last.qui", "last", 3, 1};
        statement_changed(&first);
        reject = true; statement_changed(&dropped); reject = false;
        statement_changed(&last); statement_changed(nullptr);
        reject = true;
        const bool okay = flush_report() && flush_report();
        reject = false;
        return okay ? 0 : 4;
    }
    if (mode == "--pressure") {
        std::string file(100, 'f');
        const quidra::counters::SourceSite site{file.c_str(), "node", 1, 1};
        reject = true;
        statement_changed(&site);
        reject = false;
        for (unsigned i = 0; i < 4096; ++i) {
            ProfileEvent event; event.name = "host"; event.owner = "core";
            event.host_begin_ns = 100; event.host_end_ns = 200;
            record(event);
        }
        reject = true;
        const bool okay = flush_report() && flush_report();
        reject = false;
        return okay ? 0 : 4;
    }
    std::string file = "unit\"file\n.qui", node = "node\\id", name = "kernel\"name";
    const quidra::counters::SourceSite site{file.c_str(), node.c_str(), 24, 5};
    ProfileEvent event; event.kind = ProfileEvent::Kind::Dispatch;
    event.site = site; event.owner = "core"; event.name = name.c_str(); event.kernel = "gemm";
    event.device = 0; event.stream = 2; event.host_thread = 7;
    event.host_begin_ns = 1000; event.host_end_ns = 3500;
    event.gpu_begin_ns = 4000; event.gpu_end_ns = 9500; event.bytes = 64;
    record(event); record(event);
    event.owner = "native"; event.host_begin_ns = event.host_end_ns = 0;
    event.gpu_begin_ns = event.gpu_end_ns = 0; event.bytes = 16;
    record(event);
    event.host_begin_ns = 20; event.host_end_ns = 10; record(event);
    event.host_begin_ns = 0; event.host_end_ns = 10; record(event);
    DeviceCapabilities capabilities;
    capabilities.device = 0; capabilities.backend = "test";
    capabilities.architecture = node.c_str(); capabilities.device_key = file.c_str();
    capabilities.compute_units = 4; capabilities.simd_width = 32;
    capabilities.unified_memory = true;
    capabilities.unified_memory_known = true;
    quidra::profile::note_device(capabilities);
    DeviceCapabilities unavailable;
    unavailable.device = 1;
    quidra::profile::note_device(unavailable);
    DeviceCapabilities discrete;
    discrete.device = 2; discrete.unified_memory_known = true;
    quidra::profile::note_device(discrete);
    file.assign("changed"); node.assign("changed"); name.assign("changed");
    ProfileEvent wide; wide.owner = "test"; wide.name = wide.kernel = "wide";
    wide.site = {"wide.qui", "wide", 1, 1};
    wide.host_begin_ns = 100; wide.host_end_ns = 200;
    wide.bytes = std::numeric_limits<std::uint64_t>::max();
    record(wide); record(wide);
    const auto outer = range_begin("native", "outer");
    const auto inner = range_begin("native", "inner");
    std::thread wrong([inner] { range_end(inner); }); wrong.join();
    reject = true; range_end(inner); range_end(outer); reject = false;
    range_end(inner);
    const quidra::counters::SourceSite first{"first.qui", "first", 1, 1};
    const quidra::counters::SourceSite second{"second.qui", "second", 2, 1};
    statement_changed(&first); statement_changed(&first);
    statement_changed(&second); statement_changed(nullptr);
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < 4; ++w) workers.emplace_back([] {
        ProfileEvent e; e.owner = "test"; e.name = "callback"; e.kernel = "callback";
        e.host_begin_ns = 100; e.host_end_ns = 300;
        for (unsigned i = 0; i < 25; ++i) record(e);
    });
    for (auto& w : workers) w.join();
    if (mode == "--open") (void)range_begin("native", "unclosed");
    if (mode == "--failure" || mode == "--repeat" || mode == "--open") {
        reject = true;
        const bool okay = flush_report() && flush_report();
        reject = false;
        if (!okay) return 4;
        record({});
        if (mode == "--failure") std::_Exit(101);
        return 0;
    }
    if (mode != "--records") return 2;
    std::puts("ready"); std::fflush(stdout);
    return std::getchar() == '\n' ? 0 : 2;
}
#else
int main() { std::fputs("FAIL: runtime profiler implementation is missing\n", stderr); return 1; }
#endif
