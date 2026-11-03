#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if __has_include("device/capabilities.hpp")
#include "device/capabilities.hpp"

namespace {
using namespace quidra::device;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Mock {
    std::map<CapabilityField, std::uint64_t> numbers;
    std::map<CapabilityField, std::string> texts;
    std::vector<std::optional<bool>> peers;
    unsigned calls{};

    static bool number(void* context, CapabilityField field, std::uint64_t& out) {
        auto& self = *static_cast<Mock*>(context);
        ++self.calls;
        const auto found = self.numbers.find(field);
        if (found == self.numbers.end()) return false;
        out = found->second;
        return true;
    }
    static bool text(void* context, CapabilityField field, std::string& out) {
        auto& self = *static_cast<Mock*>(context);
        ++self.calls;
        const auto found = self.texts.find(field);
        if (found == self.texts.end()) return false;
        out = found->second;
        return true;
    }
    static bool peer(void* context, std::size_t index, bool& out) {
        auto& self = *static_cast<Mock*>(context);
        ++self.calls;
        if (index >= self.peers.size() || !self.peers[index]) return false;
        out = *self.peers[index];
        return true;
    }
    CapabilitySource source() {
        return {this, &number, &text, &peer, peers.size()};
    }
};

void collected_facts() {
    Mock cuda{{{CapabilityField::ComputeUnits, 108},
               {CapabilityField::MaxThreadsPerGroup, 1024},
               {CapabilityField::SharedBytesPerGroup, 49152},
               {CapabilityField::SimdWidth, 32},
               {CapabilityField::MemoryBytes, 40ULL * 1024 * 1024 * 1024},
               {CapabilityField::UnifiedMemory, 0}},
              {{CapabilityField::Architecture, "sm_80"},
               {CapabilityField::DriverVersion, "CUDA API 12.8"},
               {CapabilityField::RuntimeVersion, "CUDA Driver API"},
               {CapabilityField::OsBuild, "test-os"}}, {false, true}};
    const auto caps = collect_capabilities(cuda.source());
    require(caps.architecture == "sm_80" && caps.compute_units == 108 &&
                caps.max_threads_per_group == 1024 && caps.shared_bytes_per_group == 49152 &&
                caps.simd_width == 32 && caps.memory_bytes == 40ULL * 1024 * 1024 * 1024,
            "typed metadata preserves units and wide memory sizes");
    require(caps.has(CapabilityField::UnifiedMemory) && !caps.unified_memory &&
                caps.has(CapabilityField::PeerAccess) && caps.peer_access == std::vector<bool>({false, true}),
            "known false flags are distinct from unavailable flags");
    Mock hip{{{CapabilityField::ComputeUnits, 120}, {CapabilityField::SimdWidth, 64}},
             {{CapabilityField::Architecture, "gfx90a:sramecc+:xnack-"}}, {false}};
    const auto partial = collect_capabilities(hip.source());
    require(partial.compute_units == 120 && partial.simd_width == 64 &&
                partial.architecture == "gfx90a:sramecc+:xnack-" &&
                !partial.has(CapabilityField::MemoryBytes) && !partial.has(CapabilityField::UnifiedMemory),
            "a partial driver query reports only supplied facts");
    auto unsupported = collect_capabilities({});
    require(unsupported.available == 0 && unsupported.peer_access.empty(),
            "missing optional query symbols leave all capabilities unavailable");
    hip.numbers[CapabilityField::ComputeUnits] = std::numeric_limits<std::uint64_t>::max();
    hip.numbers[CapabilityField::UnifiedMemory] = 2;
    hip.peers.push_back(std::nullopt);
    const auto rejected = collect_capabilities(hip.source());
    require(!rejected.has(CapabilityField::ComputeUnits) &&
                !rejected.has(CapabilityField::UnifiedMemory) &&
                !rejected.has(CapabilityField::PeerAccess) && rejected.peer_access.empty(),
            "overflow, nonboolean flags and incomplete peer queries remain unknown");
    require(hip.calls > 0 && cuda.calls > 0, "mocks exercise the production collector");
}

DeviceKeyFacts identity() {
    return {"metal", "Example Device", "test-family", "driver-1", "runtime-1", 8,
            "test-os-build", "deterministic"};
}

void stable_keys() {
    const auto facts = identity();
    const auto key = device_key(facts);
    require(key == "metal-example-device-e6c02581208d9baefdf1e67d1f62f5f8393257bf48ebb50f1eea008386d14df2",
            "framed key encoding matches the independent SHA256 oracle");
    require(device_key(facts) == key, "same identity is stable across calls");
    const auto changed = [&](auto edit) {
        auto value = facts;
        edit(value);
        require(device_key(value) != key, "identity changes invalidate the device key");
    };
    changed([](auto& f) { f.backend = "cuda"; });
    changed([](auto& f) { f.name += " new"; });
    changed([](auto& f) { f.architecture = "other-family"; });
    changed([](auto& f) { f.driver_version = "driver-2"; });
    changed([](auto& f) { f.runtime_version = "runtime-2"; });
    changed([](auto& f) { f.compute_units = 9; });
    changed([](auto& f) { f.os_build += " new"; });
    changed([](auto& f) { f.math_mode = "fast"; });
    changed([](auto& f) { f.architecture.reset(); });
    changed([](auto& f) { f.compute_units.reset(); });
    auto first = facts, second = facts;
    first.driver_version = "ab"; first.runtime_version = "c";
    second.driver_version = "a"; second.runtime_version = "bc";
    require(device_key(first) != device_key(second), "field framing prevents concatenation collisions");
    first.architecture.reset(); second = first; second.architecture = "";
    require(device_key(first) != device_key(second), "known empty text and unknown are distinct");
    for (auto member : {&DeviceKeyFacts::backend, &DeviceKeyFacts::name,
                        &DeviceKeyFacts::os_build, &DeviceKeyFacts::math_mode}) {
        auto missing = facts;
        missing.*member = "";
        require(device_key(missing).empty(), "unavailable mandatory identity produces no persistent key");
    }
    auto unusual = facts;
    unusual.name = std::string("../../Device/\xE5\x90\x8D\0", 17) + std::string(200, 'X');
    const auto safe = device_key(unusual);
    require(safe.size() <= 64 + 1 + 64 &&
                std::all_of(safe.begin(), safe.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                }), "slug is bounded and has no filesystem separators or locale bytes");
}

void fixed_test_facts() {
    const auto plain = test_capabilities(2, false);
    const auto unified = test_capabilities(2, true);
    require(plain.architecture == "test" && plain.os_build == "test" && plain.compute_units == 1 &&
                plain.max_threads_per_group == 256 && plain.shared_bytes_per_group == 32768 &&
                plain.simd_width == 32 && plain.memory_bytes == 67108864,
            "test capabilities are fixed synthetic values across hosts");
    require(plain.has(CapabilityField::UnifiedMemory) && !plain.unified_memory && unified.unified_memory &&
                plain.peer_access == std::vector<bool>({false, false}),
            "test unified state follows its explicit mock flag without implying peer access");
}

void cpu_inputs_and_snapshot() {
    using namespace quidra::device::detail;
    require(parse_cpu_list("0-3,6,8-9\n") == std::optional<std::vector<std::uint32_t>>{{0,1,2,3,6,8,9}},
            "CPU ranges expand in deterministic order");
    require(parse_cpu_list("3,1-3") == std::optional<std::vector<std::uint32_t>>{{1,2,3}},
            "overlapping sibling lists do not duplicate CPUs");
    for (const auto bad : {"", "-1", "1-0", "1,", "1--2", "1x", "4294967296", "0-4294967295"})
        require(!parse_cpu_list(bad), "malformed or unbounded CPU list is unavailable");
    require(parse_cache_bytes("32K\n") == 32768 && parse_cache_bytes("2M") == 2097152 &&
                parse_cache_bytes("1024") == 1024, "cache text converts to bytes");
    for (const auto bad : {"", "-1K", "12KiB", "1.5M", "18446744073709551616", "18446744073709551615K"})
        require(!parse_cache_bytes(bad), "cache conversion rejects malformed or overflowing sizes");
    const auto& cpu = cpu_capabilities();
    require(&cpu_capabilities() == &cpu, "CPU metadata is one immutable process snapshot");
    if (cpu.physical_cpus && cpu.logical_cpus)
        require(*cpu.physical_cpus > 0 && *cpu.physical_cpus <= *cpu.logical_cpus,
                "reported physical/logical topology is coherent");
    if (cpu.simd_width_bits) require(*cpu.simd_width_bits > 0, "known SIMD width is in bits");
}

void cpu_topology_fixtures() {
    using namespace quidra::device::detail;
    std::vector<CpuSample> threads{
        {0, 0, 10, "performance", 65536, 4194304},
        {1, 0, 10, "performance", 65536, 4194304},
        {2, 0, 20, "efficiency", 32768, 2097152},
    };
    threads.push_back(threads.front());
    const auto mixed = summarize_cpu_samples(threads);
    require(mixed.logical_cpus == 3 && mixed.physical_cpus == 2 && mixed.core_groups.size() == 2 &&
                !mixed.l1_data_cache_bytes,
            "siblings/duplicate records are not extra cores and heterogeneous cache sizes stay explicit");
    const auto performance = std::find_if(mixed.core_groups.begin(), mixed.core_groups.end(),
        [](const auto& group) { return group.label == "performance"; });
    require(performance != mixed.core_groups.end() && performance->physical_cpus == 1 &&
                performance->logical_cpus == 2 && performance->l2_cache_bytes == 4194304,
            "reported core classes retain their own counts and shared-cache bytes");
    threads.pop_back();
    threads[1].l2_cache_bytes.reset();
    threads[1].core.reset();
    const auto partial = summarize_cpu_samples(threads);
    const auto group = std::find_if(partial.core_groups.begin(), partial.core_groups.end(),
        [](const auto& value) { return value.label == "performance"; });
    require(partial.logical_cpus == 3 && !partial.physical_cpus && group != partial.core_groups.end() &&
                !group->physical_cpus && !group->l2_cache_bytes,
            "one missing sibling identity/cache query cannot become a complete group fact");
    threads.push_back(threads.front());
    threads.back().core = 99;
    const auto conflict = summarize_cpu_samples(threads);
    require(!conflict.logical_cpus && !conflict.physical_cpus && conflict.core_groups.empty(),
            "conflicting records for one logical CPU are unavailable rather than arbitrarily selected");
}
} // namespace
#endif

int main() {
#if __has_include("device/capabilities.hpp")
    try {
        collected_facts();
        stable_keys();
        fixed_test_facts();
        cpu_inputs_and_snapshot();
        cpu_topology_fixtures();
        std::cout << "device capabilities: collection, keys, CPU metadata verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#else
    std::cerr << "device capability model is not implemented\n";
    return 1;
#endif
}
