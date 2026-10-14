#include "device/capabilities.hpp"

#include "platform/os_version.hpp"
#include "platform/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// The Windows printer API alias must not rewrite our metadata type names.
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#endif

namespace quidra::device {
namespace {

void mark(DeviceCapabilities& caps, CapabilityField field) {
    caps.available |= static_cast<std::uint32_t>(field);
}

std::string_view trimmed(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

std::optional<std::uint64_t> unsigned_text(std::string_view text) {
    text = trimmed(text);
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

std::optional<std::uint32_t> count(std::optional<std::uint64_t> value) {
    if (!value || *value == 0 || *value > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return static_cast<std::uint32_t>(*value);
}

std::string decimal(std::uint64_t value) {
    std::array<char, 20> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), result.ptr};
}

void frame(std::string& encoded, std::string_view value) {
    encoded += decimal(value.size()); encoded += ':'; encoded += value;
}

void optional_frame(std::string& encoded, const std::optional<std::string>& value) {
    frame(encoded, value ? "1" : "0");
    if (value) frame(encoded, *value);
}

std::string slug(std::string_view backend, std::string_view name) {
    std::string result;
    const auto append = [&](std::string_view text) {
        for (unsigned char c : text) {
            if (result.size() == 64) break;
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) result += static_cast<char>(c);
            else if (!result.empty() && result.back() != '-') result += '-';
        }
    };
    append(backend);
    if (!result.empty() && result.back() != '-' && result.size() < 64) result += '-';
    append(name);
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result.empty() ? "device" : result;
}

#if defined(__linux__)
std::string file_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string text;
    if (!file || !std::getline(file, text)) return {};
    return text;
}
#endif

std::optional<std::uint64_t> common_size(const std::set<std::uint64_t>& values) {
    if (values.size() != 1 || *values.begin() == 0) return std::nullopt;
    return *values.begin();
}

#ifdef __APPLE__
std::optional<std::uint64_t> sysctl_unsigned(const char* name) {
    std::array<unsigned char, 8> bytes{};
    std::size_t size = bytes.size();
    if (::sysctlbyname(name, bytes.data(), &size, nullptr, 0) != 0) return std::nullopt;
    if (size == sizeof(std::uint32_t)) {
        std::uint32_t value{}; std::memcpy(&value, bytes.data(), sizeof(value)); return value;
    }
    if (size == sizeof(std::uint64_t)) {
        std::uint64_t value{}; std::memcpy(&value, bytes.data(), sizeof(value)); return value;
    }
    return std::nullopt;
}

std::string sysctl_string(const char* name) {
    std::size_t size{};
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0 || size > 65536) return {};
    std::string text(size, '\0');
    if (::sysctlbyname(name, text.data(), &size, nullptr, 0) != 0) return {};
    text.resize(size);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}
#endif

#ifdef _WIN32
template<class T>
std::string efficiency_group(const T& processor) {
    if constexpr (requires { processor.EfficiencyClass; })
        return "class-" + decimal(processor.EfficiencyClass);
    return "all";
}
#endif

CpuCapabilities probe_cpu() {
    CpuCapabilities caps;
    caps.os_build = platform::os_version();
#if defined(__aarch64__) || defined(_M_ARM64)
    caps.architecture = "aarch64";
    caps.simd_width_bits = 128;
#elif defined(__x86_64__) || defined(_M_X64)
    caps.architecture = "x86_64";
    caps.simd_width_bits = 128; // SSE2 is part of the executable ABI.
#elif defined(__i386__) || defined(_M_IX86)
    caps.architecture = "x86";
#endif
#ifndef _WIN32
    if (caps.architecture.empty()) {
        struct utsname machine {};
        if (::uname(&machine) == 0) caps.architecture = machine.machine;
    }
#endif

#ifdef __APPLE__
    if (caps.architecture.empty()) caps.architecture = sysctl_string("hw.machine");
    caps.model = sysctl_string("machdep.cpu.brand_string");
    if (caps.model.empty()) caps.model = sysctl_string("hw.model");
    caps.physical_cpus = count(sysctl_unsigned("hw.physicalcpu"));
    caps.logical_cpus = count(sysctl_unsigned("hw.logicalcpu"));
    caps.l1_data_cache_bytes = sysctl_unsigned("hw.l1dcachesize");
    if (caps.l1_data_cache_bytes == 0) caps.l1_data_cache_bytes.reset();
    for (unsigned i = 0; i < 2; ++i) {
        const auto prefix = "hw.perflevel" + decimal(i);
        CpuCoreGroup group;
        group.label = i == 0 ? "performance" : "efficiency";
        group.physical_cpus = count(sysctl_unsigned((prefix + ".physicalcpu").c_str()));
        group.logical_cpus = count(sysctl_unsigned((prefix + ".logicalcpu").c_str()));
        group.l2_cache_bytes = sysctl_unsigned((prefix + ".l2cachesize").c_str());
        if (group.l2_cache_bytes == 0) group.l2_cache_bytes.reset();
        if (group.physical_cpus || group.logical_cpus) caps.core_groups.push_back(std::move(group));
    }
    if (caps.core_groups.empty() && (caps.physical_cpus || caps.logical_cpus))
        caps.core_groups.push_back({"all", caps.physical_cpus, caps.logical_cpus, sysctl_unsigned("hw.l2cachesize")});
#elif defined(__linux__)
    const std::filesystem::path root = "/sys/devices/system/cpu";
    const auto online = detail::parse_cpu_list(file_text(root / "online"));
    if (online) {
        std::vector<detail::CpuSample> samples;
        for (const auto cpu : *online) {
            const auto path = root / ("cpu" + decimal(cpu));
            detail::CpuSample sample;
            sample.logical_id = cpu;
            sample.package = unsigned_text(file_text(path / "topology/physical_package_id"));
            sample.core = unsigned_text(file_text(path / "topology/core_id"));
            const auto type = unsigned_text(file_text(path / "topology/core_type"));
            sample.group = type ? "class-" + decimal(*type) : "unclassified";
            std::set<std::uint64_t> l1, l2;
            std::error_code error;
            const auto cache = path / "cache";
            std::filesystem::directory_iterator entries(cache, error);
            while (!error && entries != std::filesystem::directory_iterator{}) {
                const auto& entry = *entries;
                const auto level = unsigned_text(file_text(entry.path() / "level"));
                const auto kind = file_text(entry.path() / "type");
                const auto size = detail::parse_cache_bytes(file_text(entry.path() / "size"));
                if (level && size && *size != 0 && (kind == "Data" || kind == "Unified")) {
                    if (*level == 1) l1.insert(*size);
                    if (*level == 2) l2.insert(*size);
                }
                entries.increment(error);
            }
            if (!error) { sample.l1_data_cache_bytes = common_size(l1); sample.l2_cache_bytes = common_size(l2); }
            samples.push_back(std::move(sample));
        }
        auto topology = detail::summarize_cpu_samples(samples);
        caps.physical_cpus = topology.physical_cpus; caps.logical_cpus = topology.logical_cpus;
        caps.l1_data_cache_bytes = topology.l1_data_cache_bytes;
        caps.core_groups = std::move(topology.core_groups);
    }
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = trimmed(std::string_view(line).substr(0, colon));
        if (key == "model name" || key == "Hardware") {
            caps.model = trimmed(std::string_view(line).substr(colon + 1)); break;
        }
    }
#elif defined(_WIN32)
    DWORD bytes{};
    (void)GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
    if (bytes != 0 && bytes <= 16U * 1024 * 1024) {
        std::vector<unsigned char> data(bytes);
        if (GetLogicalProcessorInformationEx(RelationAll,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()), &bytes)) {
            std::uint64_t physical{}, logical{};
            std::set<std::uint64_t> l1, l2;
            std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> groups;
            bool complete = true;
            for (std::size_t offset = 0; offset < bytes;) {
                const auto* record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(data.data() + offset);
                if (bytes - offset < 2 * sizeof(DWORD) || record->Size < 2 * sizeof(DWORD) || record->Size > bytes - offset) {
                    complete = false; break;
                }
                if (record->Relationship == RelationProcessorCore) {
                    const auto minimum = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) +
                        offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
                    if (record->Size < minimum || record->Processor.GroupCount > (record->Size - minimum) / sizeof(GROUP_AFFINITY)) {
                        complete = false; break;
                    }
                    ++physical;
                    std::uint64_t core_logical{};
                    for (WORD g = 0; g < record->Processor.GroupCount; ++g)
                        core_logical += std::popcount(static_cast<std::uint64_t>(record->Processor.GroupMask[g].Mask));
                    logical += core_logical;
                    auto& group = groups[efficiency_group(record->Processor)];
                    ++group.first; group.second += core_logical;
                } else if (record->Relationship == RelationCache &&
                           record->Size >= offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) + sizeof(CACHE_RELATIONSHIP)) {
                    if (record->Cache.Type == CacheData || record->Cache.Type == CacheUnified) {
                        if (record->Cache.Level == 1) l1.insert(record->Cache.CacheSize);
                        if (record->Cache.Level == 2) l2.insert(record->Cache.CacheSize);
                    }
                }
                offset += record->Size;
            }
            if (complete) {
                caps.physical_cpus = count(physical); caps.logical_cpus = count(logical);
                caps.l1_data_cache_bytes = common_size(l1);
                for (const auto& [label, group] : groups)
                    caps.core_groups.push_back({label, count(group.first), count(group.second),
                                                groups.size() == 1 ? common_size(l2) : std::nullopt});
            }
        }
    }
#endif
    return caps;
}

} // namespace

DeviceCapabilities collect_capabilities(const CapabilitySource& source) {
    DeviceCapabilities caps;
    const auto number = [&](CapabilityField field) -> std::optional<std::uint64_t> {
        std::uint64_t value{};
        if (!source.read_unsigned || !source.read_unsigned(source.context, field, value)) return std::nullopt;
        return value;
    };
    const auto narrow = [&](CapabilityField field, std::uint32_t& destination, bool allow_zero = false) {
        const auto value = number(field);
        if (!value || (!allow_zero && *value == 0) || *value > std::numeric_limits<std::uint32_t>::max()) return;
        destination = static_cast<std::uint32_t>(*value); mark(caps, field);
    };
    narrow(CapabilityField::ComputeUnits, caps.compute_units);
    narrow(CapabilityField::MaxThreadsPerGroup, caps.max_threads_per_group);
    narrow(CapabilityField::SharedBytesPerGroup, caps.shared_bytes_per_group, true);
    narrow(CapabilityField::SimdWidth, caps.simd_width);
    if (const auto value = number(CapabilityField::MemoryBytes); value && *value != 0) {
        caps.memory_bytes = *value; mark(caps, CapabilityField::MemoryBytes);
    }
    if (const auto value = number(CapabilityField::UnifiedMemory); value && *value <= 1) {
        caps.unified_memory = *value != 0; mark(caps, CapabilityField::UnifiedMemory);
    }
    const auto text = [&](CapabilityField field, std::string& destination) {
        std::string value;
        if (!source.read_text || !source.read_text(source.context, field, value) || value.empty()) return;
        destination = std::move(value); mark(caps, field);
    };
    text(CapabilityField::Architecture, caps.architecture);
    text(CapabilityField::DriverVersion, caps.driver_version);
    text(CapabilityField::RuntimeVersion, caps.runtime_version);
    text(CapabilityField::OsBuild, caps.os_build);
    if (source.read_peer) {
        std::vector<bool> peers;
        for (std::size_t i = 0; i < source.device_count; ++i) {
            bool peer{};
            if (!source.read_peer(source.context, i, peer)) return caps;
            peers.push_back(peer);
        }
        caps.peer_access = std::move(peers); mark(caps, CapabilityField::PeerAccess);
    }
    return caps;
}

DeviceCapabilities test_capabilities(std::size_t device_count, bool unified_memory) {
    DeviceCapabilities caps;
    caps.architecture = "test"; caps.compute_units = 1; caps.max_threads_per_group = 256;
    caps.shared_bytes_per_group = 32768; caps.simd_width = 32; caps.memory_bytes = 67108864;
    caps.unified_memory = unified_memory; caps.peer_access.assign(device_count, false);
    caps.driver_version = "test"; caps.runtime_version = "test"; caps.os_build = "test";
    caps.available = (1U << 11) - 1;
    return caps;
}

std::string device_key(const DeviceKeyFacts& facts) {
    if (facts.backend.empty() || facts.name.empty() || facts.os_build.empty() || facts.math_mode.empty()) return {};
    std::string encoded;
    frame(encoded, "quidra-device-key-v1"); frame(encoded, facts.backend); frame(encoded, facts.name);
    optional_frame(encoded, facts.architecture); optional_frame(encoded, facts.driver_version);
    optional_frame(encoded, facts.runtime_version); frame(encoded, facts.compute_units ? "1" : "0");
    if (facts.compute_units) frame(encoded, decimal(*facts.compute_units));
    frame(encoded, facts.os_build); frame(encoded, facts.math_mode);
    platform::Sha256 hash(platform::Sha256Engine::portable);
    hash.update(encoded);
    return slug(facts.backend, facts.name) + '-' + hash.finish_hex();
}

const CpuCapabilities& cpu_capabilities() {
    static const CpuCapabilities value = probe_cpu();
    return value;
}

namespace detail {

CpuCapabilities summarize_cpu_samples(const std::vector<CpuSample>& samples) {
    CpuCapabilities caps;
    std::map<std::uint32_t, const CpuSample*> logical;
    for (const auto& sample : samples) {
        const auto [where, inserted] = logical.emplace(sample.logical_id, &sample);
        if (!inserted && *where->second != sample) return caps;
    }
    struct Group {
        std::uint64_t logical{};
        std::set<std::pair<std::uint64_t, std::uint64_t>> physical;
        std::set<std::uint64_t> l2;
        bool physical_complete{true}, l2_complete{true};
    };
    std::map<std::string, Group> groups;
    std::set<std::pair<std::uint64_t, std::uint64_t>> physical;
    std::set<std::uint64_t> l1;
    bool physical_complete = true, l1_complete = true;
    for (const auto& [id, sample] : logical) {
        (void)id;
        auto& group = groups[sample->group.empty() ? "unclassified" : sample->group];
        ++group.logical;
        if (sample->package && sample->core) {
            physical.emplace(*sample->package, *sample->core);
            group.physical.emplace(*sample->package, *sample->core);
        } else { physical_complete = false; group.physical_complete = false; }
        if (sample->l1_data_cache_bytes && *sample->l1_data_cache_bytes != 0) l1.insert(*sample->l1_data_cache_bytes);
        else l1_complete = false;
        if (sample->l2_cache_bytes && *sample->l2_cache_bytes != 0) group.l2.insert(*sample->l2_cache_bytes);
        else group.l2_complete = false;
    }
    caps.logical_cpus = count(logical.size());
    if (physical_complete) caps.physical_cpus = count(physical.size());
    if (l1_complete) caps.l1_data_cache_bytes = common_size(l1);
    for (const auto& [label, group] : groups)
        caps.core_groups.push_back({label, group.physical_complete ? count(group.physical.size()) : std::nullopt,
                                    count(group.logical), group.l2_complete ? common_size(group.l2) : std::nullopt});
    return caps;
}

std::optional<std::vector<std::uint32_t>> parse_cpu_list(std::string_view text) {
    text = trimmed(text);
    if (text.empty()) return std::nullopt;
    // Bound expansion, not CPU identifiers: topology reports must not allocate
    // billions of entries when an OS file is malformed or a test supplies it.
    constexpr std::size_t maximum_entries = 65536;
    std::set<std::uint32_t> cpus;
    for (;;) {
        const auto comma = text.find(',');
        const auto part = trimmed(text.substr(0, comma));
        const auto dash = part.find('-');
        const auto first = unsigned_text(part.substr(0, dash));
        const auto last = dash == std::string_view::npos ? first : unsigned_text(part.substr(dash + 1));
        if (!first || !last || *first > *last || *last > std::numeric_limits<std::uint32_t>::max() ||
            *last - *first >= maximum_entries) return std::nullopt;
        for (std::uint64_t cpu = *first; cpu <= *last; ++cpu) {
            cpus.insert(static_cast<std::uint32_t>(cpu));
            if (cpus.size() > maximum_entries) return std::nullopt;
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty()) return std::nullopt;
    }
    return std::vector<std::uint32_t>(cpus.begin(), cpus.end());
}

std::optional<std::uint64_t> parse_cache_bytes(std::string_view text) {
    text = trimmed(text);
    if (text.empty()) return std::nullopt;
    std::uint64_t multiplier = 1;
    if (text.back() == 'K' || text.back() == 'M' || text.back() == 'G') {
        multiplier = text.back() == 'K' ? 1024ULL : text.back() == 'M' ? 1048576ULL : 1073741824ULL;
        text.remove_suffix(1);
    }
    const auto value = unsigned_text(text);
    if (!value || *value > std::numeric_limits<std::uint64_t>::max() / multiplier) return std::nullopt;
    return *value * multiplier;
}

} // namespace detail
} // namespace quidra::device
