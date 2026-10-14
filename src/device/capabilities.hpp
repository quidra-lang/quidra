#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::device {

enum class CapabilityField : std::uint32_t {
    Architecture = 1U << 0,
    ComputeUnits = 1U << 1,
    MaxThreadsPerGroup = 1U << 2,
    SharedBytesPerGroup = 1U << 3,
    SimdWidth = 1U << 4,
    MemoryBytes = 1U << 5,
    UnifiedMemory = 1U << 6,
    PeerAccess = 1U << 7,
    DriverVersion = 1U << 8,
    RuntimeVersion = 1U << 9,
    OsBuild = 1U << 10,
};

// Missing queries leave a field unavailable. In particular, the stored false
// or zero default is not a fact until has(field) is true. SIMD width counts
// GPU lanes; CPU SIMD width below uses bits. Memory is total device memory,
// or the recommended working set on Metal, never currently free memory.
struct DeviceCapabilities {
    std::string architecture;
    std::uint32_t compute_units{}, max_threads_per_group{}, shared_bytes_per_group{}, simd_width{};
    std::uint64_t memory_bytes{};
    bool unified_memory{};
    std::vector<bool> peer_access;
    std::string driver_version, runtime_version, os_build;
    std::uint32_t available{};

    bool has(CapabilityField field) const {
        return (available & static_cast<std::uint32_t>(field)) != 0;
    }
};

// Borrowed, synchronous metadata callbacks. No callback activates a context,
// creates a queue/pipeline, allocates a GPU buffer or submits/waits for work.
// The driver adapter uses supported API symbols and layouts. A missing or
// failed optional query affects its field, not backend readiness.
struct CapabilitySource {
    void* context{};
    bool (*read_unsigned)(void*, CapabilityField, std::uint64_t&){};
    bool (*read_text)(void*, CapabilityField, std::string&){};
    bool (*read_peer)(void*, std::size_t, bool&){};
    std::size_t device_count{};
};

DeviceCapabilities collect_capabilities(const CapabilitySource& source);
DeviceCapabilities test_capabilities(std::size_t device_count, bool unified_memory);

// Identity, not an enumeration position or runtime handle. Callers supply a
// canonical backend spelling and the actual library math mode. Optional facts
// retain their unknown marker in the key. OS build and mode are mandatory.
struct DeviceKeyFacts {
    std::string backend, name;
    std::optional<std::string> architecture, driver_version, runtime_version;
    std::optional<std::uint32_t> compute_units;
    std::string os_build, math_mode;
};

// Pure, locale-independent, length-framed SHA256. Returns empty when mandatory
// identity is missing. This function performs no discovery or cache write.
std::string device_key(const DeviceKeyFacts& facts);

struct CpuCoreGroup {
    std::string label;
    std::optional<std::uint32_t> physical_cpus, logical_cpus;
    std::optional<std::uint64_t> l2_cache_bytes;
};

struct CpuCapabilities {
    std::string architecture, model, os_build;
    std::optional<std::uint32_t> physical_cpus, logical_cpus;
    std::optional<std::uint64_t> l1_data_cache_bytes;
    // Baseline SIMD available to this executable; this is not a promise of a
    // maximum vector ISA or an instruction-selection policy.
    std::optional<std::uint32_t> simd_width_bits;
    std::vector<CpuCoreGroup> core_groups;
};

// One immutable OS snapshot. Unknown counts are not replaced by the runtime's
// worker count, its clamp, or a fabricated single-core fallback.
const CpuCapabilities& cpu_capabilities();

namespace detail {
// OS adapters normalize each logical CPU without inferring a class from its
// index. Repeated sibling/cache records may describe the same physical core.
struct CpuSample {
    std::uint32_t logical_id{};
    std::optional<std::uint64_t> package, core;
    std::string group;
    std::optional<std::uint64_t> l1_data_cache_bytes, l2_cache_bytes;
    bool operator==(const CpuSample&) const = default;
};
CpuCapabilities summarize_cpu_samples(const std::vector<CpuSample>& samples);
std::optional<std::vector<std::uint32_t>> parse_cpu_list(std::string_view text);
std::optional<std::uint64_t> parse_cache_bytes(std::string_view text);
} // namespace detail

} // namespace quidra::device
