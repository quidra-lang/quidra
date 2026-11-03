#include "device_cli.hpp"
#include "device_backend.hpp"
#include "device/capabilities.hpp"
#include "quidra/project.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::vector<quidra::device::Info> listed;
unsigned capability_queries{}, key_queries{}, cpu_queries{}, metadata_requests{};
bool verbose_call{};
bool known_cpu{true};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string capture(bool verbose) {
    verbose_call = verbose;
    std::ostringstream output;
    struct Restore {
        std::streambuf* old;
        ~Restore() { std::cout.rdbuf(old); }
    } restore{std::cout.rdbuf(output.rdbuf())};
    require(quidra::cli::run_gpu_cli(verbose) == 0, "device listing status changed");
    return output.str();
}
void reset() { capability_queries = key_queries = cpu_queries = metadata_requests = 0; }
} // namespace

// Only discovery/display and metadata functions are provided. A CLI path that
// acquires handles, allocates, launches or synchronizes cannot link this test.
namespace quidra::device {
void request_capability_metadata() noexcept { ++metadata_requests; }
const std::vector<Info>& devices() {
    require(!verbose_call || metadata_requests != 0, "verbose metadata must be requested before discovery");
    return listed;
}
std::string backend_display_name(Backend backend) { return backend == Backend::Metal ? "Metal" : "AMD"; }
DeviceCapabilities capabilities(int index) {
    ++capability_queries;
    DeviceCapabilities caps;
    if (index != 37) return caps;
    caps.architecture = "example-family";
    caps.compute_units = 8;
    caps.max_threads_per_group = 256;
    caps.shared_bytes_per_group = 32768;
    caps.memory_bytes = 67108864;
    caps.unified_memory = false;
    caps.peer_access = {false, true};
    caps.driver_version = "1.0";
    caps.os_build = "example-os";
    for (const auto field : {CapabilityField::Architecture, CapabilityField::ComputeUnits,
            CapabilityField::MaxThreadsPerGroup, CapabilityField::SharedBytesPerGroup,
            CapabilityField::MemoryBytes, CapabilityField::UnifiedMemory, CapabilityField::PeerAccess,
            CapabilityField::DriverVersion, CapabilityField::OsBuild})
        caps.available |= static_cast<std::uint32_t>(field);
    return caps;
}
std::string device_key(int index) { ++key_queries; return index == 37 ? "metal-example-key" : ""; }
const CpuCapabilities& cpu_capabilities() {
    ++cpu_queries;
    static const CpuCapabilities known{"example-cpu", "Example CPU", "example-os", 4, 8, 65536, 128,
        {{"primary", 2, 4, 1048576}, {"optional", {}, {}, {}}}};
    static const CpuCapabilities unknown;
    return known_cpu ? known : unknown;
}
} // namespace quidra::device

int main() {
    try {
        using namespace quidra;
        using namespace quidra::device;
        listed = {{37, Backend::Metal, 0, "Example GPU", "", "runtime-info"},
                  {91, Backend::Hip, 0, "Optional GPU", "", ""}};
        std::ostringstream expected;
        for (const auto& gpu : listed) {
            expected << "GPU " << gpu.index << "\n  name: " << gpu.name
                     << "\n  backend: " << backend_display_name(gpu.backend) << '\n';
            if (!gpu.runtime.empty()) expected << "  runtime: " << gpu.runtime << '\n';
            expected << "  " << language_name << " version: " << version
                     << "\n  status: supported\n  placement: explicit\n  implicit CPU fallback: disabled\n";
        }
        reset();
        require(capture(false) == expected.str(), "plain GPU output changed");
        require(!capability_queries && !key_queries && !cpu_queries && !metadata_requests,
                "plain listing queried verbose metadata");
        const auto verbose = capture(true);
        require(capability_queries == 2 && key_queries == 2 && cpu_queries == 1 && metadata_requests == 1,
                "verbose listing did not query metadata exactly once per device/snapshot");
        for (const auto token : {"CPU capabilities:", "architecture: example-cpu", "physical CPUs: 4",
                "logical CPUs: 8", "baseline SIMD width (bits): 128", "L1 data cache (bytes): 65536",
                "core group primary", "L2 cache (bytes): 1048576", "core group optional",
                "architecture: example-family", "compute units: 8", "max threads per group: 256",
                "shared memory per group (bytes): 32768", "SIMD width (lanes): unknown",
                "recommended working set (bytes): 67108864", "unified memory: no",
                "peer access: [no, yes]", "runtime version: unknown", "device key: metal-example-key",
                "device key: unknown", "compute units: unknown", "unified memory: unknown"})
            require(verbose.find(token) != std::string::npos, "verbose metadata missing or fabricated");
        listed.clear(); reset();
        require(capture(false) == "No supported GPUs found.\n" && !cpu_queries,
                "plain empty listing changed");
        known_cpu = false;
        const auto empty = capture(true);
        require(empty.find("CPU capabilities:") != std::string::npos &&
                    empty.find("physical CPUs: unknown") != std::string::npos &&
                    empty.find("No supported GPUs found.") != std::string::npos &&
                    cpu_queries == 1 && !capability_queries && !key_queries,
                "no-GPU verbose listing lost CPU metadata or invented unknown facts");
        std::cout << "device CLI: stable plain output and truthful verbose metadata verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
