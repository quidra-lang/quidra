#include "device_cli.hpp"
#include "device_backend.hpp"
#include "runtime/device_observation.hpp"
#include "quidra/project.hpp"

#include <iostream>

namespace quidra::cli {
namespace {

std::string_view known_text(const std::string& value) {
    return value.empty() ? std::string_view("unknown") : std::string_view(value);
}

template<class T>
void cpu_number(std::string_view label, const std::optional<T>& value, std::string_view indent = "  ") {
    std::cout << indent << label << ": ";
    if (value) std::cout << *value;
    else std::cout << "unknown";
    std::cout << '\n';
}

void print_cpu_capabilities() {
    const auto& cpu = device::cpu_capabilities();
    std::cout << "CPU capabilities:\n";
    std::cout << "  architecture: " << known_text(cpu.architecture) << '\n';
    std::cout << "  model: " << known_text(cpu.model) << '\n';
    std::cout << "  OS build: " << known_text(cpu.os_build) << '\n';
    cpu_number("physical CPUs", cpu.physical_cpus);
    cpu_number("logical CPUs", cpu.logical_cpus);
    cpu_number("baseline SIMD width (bits)", cpu.simd_width_bits);
    cpu_number("L1 data cache (bytes)", cpu.l1_data_cache_bytes);
    if (cpu.core_groups.empty()) std::cout << "  core groups: unknown\n";
    for (const auto& group : cpu.core_groups) {
        std::cout << "  core group " << known_text(group.label) << ":\n";
        cpu_number("physical CPUs", group.physical_cpus, "    ");
        cpu_number("logical CPUs", group.logical_cpus, "    ");
        cpu_number("L2 cache (bytes)", group.l2_cache_bytes, "    ");
    }
}

void print_gpu_capabilities(const device::Info& gpu) {
    const auto caps = device::capabilities(gpu.index);
    const auto text = [&](std::string_view label, device::CapabilityField field, const std::string& value) {
        std::cout << "  " << label << ": " << (caps.has(field) ? known_text(value) : "unknown") << '\n';
    };
    const auto number = [&](std::string_view label, device::CapabilityField field, std::uint64_t value) {
        std::cout << "  " << label << ": ";
        if (caps.has(field)) std::cout << value;
        else std::cout << "unknown";
        std::cout << '\n';
    };
    text("architecture", device::CapabilityField::Architecture, caps.architecture);
    number("compute units", device::CapabilityField::ComputeUnits, caps.compute_units);
    number("max threads per group", device::CapabilityField::MaxThreadsPerGroup, caps.max_threads_per_group);
    number("shared memory per group (bytes)", device::CapabilityField::SharedBytesPerGroup, caps.shared_bytes_per_group);
    number("SIMD width (lanes)", device::CapabilityField::SimdWidth, caps.simd_width);
    number(gpu.backend == device::Backend::Metal ? "recommended working set (bytes)" : "device memory (bytes)",
           device::CapabilityField::MemoryBytes, caps.memory_bytes);
    std::cout << "  unified memory: " << (caps.has(device::CapabilityField::UnifiedMemory) ?
        (caps.unified_memory ? "yes" : "no") : "unknown") << '\n';
    std::cout << "  peer access: ";
    if (!caps.has(device::CapabilityField::PeerAccess)) std::cout << "unknown\n";
    else {
        std::cout << '[';
        for (std::size_t i = 0; i < caps.peer_access.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << (caps.peer_access[i] ? "yes" : "no");
        }
        std::cout << "] (listed GPU order)\n";
    }
    text("driver version", device::CapabilityField::DriverVersion, caps.driver_version);
    text("runtime version", device::CapabilityField::RuntimeVersion, caps.runtime_version);
    text("OS build", device::CapabilityField::OsBuild, caps.os_build);
    std::cout << "  device key: " << known_text(device::device_key(gpu.index)) << '\n';
}

} // namespace

int run_gpu_cli(bool verbose) {
    if (verbose) device::request_capability_metadata();
    const auto& all = device::devices();
    if (verbose) {
        std::cout << quidra::language_name << " " << quidra::version << "\n";
        std::cout << "CPU backend: native LLVM\n";
        print_cpu_capabilities();
    }
    if (all.empty()) {
        std::cout << "No supported GPUs found.\n";
        return 0;
    }

    for (const auto& gpu : all) {
        std::cout << "GPU " << gpu.index << "\n";
        std::cout << "  name: " << gpu.name << "\n";
        std::cout << "  backend: " << device::backend_display_name(gpu.backend) << "\n";
        if (!gpu.driver.empty())
            std::cout << "  driver: " << gpu.driver << "\n";
        if (!gpu.runtime.empty())
            std::cout << "  runtime: " << gpu.runtime << "\n";
        std::cout << "  " << quidra::language_name << " version: "
                  << quidra::version << "\n";
        if (gpu.backend == device::Backend::Cuda) {
            std::cout << "  CUDA Toolkit dependency: none (CUDA Driver API only)\n";
        }
        std::cout << "  status: supported\n";
        std::cout << "  placement: explicit\n";
        std::cout << "  implicit CPU fallback: disabled\n";
        if (verbose) print_gpu_capabilities(gpu);
    }
    return 0;
}

} // namespace quidra::cli
