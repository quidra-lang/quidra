#include "runtime/timeline.hpp"
#include "platform/clock.hpp"
#include "platform/environment.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <share.h>
#endif

namespace quidra::timeline {
namespace detail {
std::atomic<bool> enabled{false};
std::atomic<bool> time_steps{false};
std::atomic<std::uint32_t> step{0};
}
namespace {

struct Source {
    std::string file, node_id;
    unsigned long long line{}, column{};
};
template<class T> struct Record {
    T value;
    Source source;
    std::string text;
};
struct DeviceRecord {
    DeviceCapabilities value;
    std::string backend, architecture, device_key, driver, os_build;
};
struct Report {
    std::mutex mutex;
    std::string path;
    bool configured{}, program{}, finalized{}, written{}, runtime_remembered{};
    bool first_statement{}, exit_begun{}, exit_ended{};
    std::uint64_t process_start{}, runtime_init{}, runtime_cpu{};
    std::uint64_t invalid_drops{};
    std::atomic<std::uint64_t> storage_drops{0};
    std::vector<Record<HostEvent>> hosts;
    std::vector<Record<WaitInterval>> waits;
    std::vector<Record<GpuInterval>> gpu;
    std::vector<Record<GpuStepInterval>> gpu_steps;
    std::vector<Record<GpuSampling>> sampling;
    std::vector<Record<GpuSamplingGap>> sampling_gaps;
    std::vector<Record<WarmupInterval>> warmups;
    std::vector<Record<CompileInterval>> compiles;
    std::vector<DeviceRecord> devices;
};

Report& report() {
    static Report state;
    return state;
}

struct StreamLock {
    std::FILE* file;
    explicit StreamLock(std::FILE* value) noexcept : file(value) {
#ifdef _WIN32
        _lock_file(file);
#else
        ::flockfile(file);
#endif
    }
    ~StreamLock() {
#ifdef _WIN32
        _unlock_file(file);
#else
        ::funlockfile(file);
#endif
    }
};

const char* point_name(Point point) {
    switch (point) {
    case Point::ProcessStart: return "process_start";
    case Point::RuntimeInit: return "runtime_init";
    case Point::FirstStatement: return "first_statement";
    case Point::DeviceInitBegin: return "device_init_begin";
    case Point::DeviceInitEnd: return "device_init_end";
    case Point::StepBoundary: return "step_boundary";
    case Point::TimeMark: return "time_mark";
    case Point::ExitBegin: return "exit_begin";
    case Point::ExitEnd: return "exit_end";
    }
    return nullptr;
}
const char* reason_name(counters::WaitReason reason) {
    switch (reason) {
    case counters::WaitReason::Sync: return "sync";
    case counters::WaitReason::Read: return "read";
    case counters::WaitReason::Status: return "status";
    }
    return nullptr;
}
const char* host_clock_name() {
#if defined(__APPLE__)
    return "mach_absolute_ns";
#elif defined(__linux__)
    return "clock_monotonic_ns";
#elif defined(_WIN32)
    return "qpc_ns";
#else
    return "unavailable";
#endif
}

bool active(const Report& state) {
    return state.program && !state.finalized && !state.path.empty();
}
bool ordered_pair(std::uint64_t begin, std::uint64_t end) {
    return (begin == 0 && end == 0) || (begin != 0 && end >= begin);
}
bool valid(const HostEvent& event) { return point_name(event.point) != nullptr; }
bool valid(const WaitInterval& event) {
    return reason_name(event.reason) != nullptr && ordered_pair(event.begin_ns, event.end_ns);
}
bool valid(const WarmupInterval& event) { return ordered_pair(event.begin_ns, event.end_ns); }
bool valid(const CompileInterval& event) { return ordered_pair(event.begin_ns, event.end_ns); }
bool valid(const GpuInterval& event) {
    return ordered_pair(event.scheduled_begin_ns, event.scheduled_end_ns) &&
           ordered_pair(event.gpu_begin_ns, event.gpu_end_ns);
}
bool valid(const GpuStepInterval& event) { return ordered_pair(event.gpu_begin_ns, event.gpu_end_ns); }
bool valid(const GpuSampling&) { return true; }
bool valid(const GpuSamplingGap&) { return true; }

Source copy_source(const counters::SourceSite& site) {
    return {site.file ? site.file : "", site.node_id ? site.node_id : "", site.line, site.column};
}
counters::SourceSite source_site(const counters::SourceSite* site) {
    if (site) return *site;
    counters::SourceSite current{};
    (void)counters::current_source_site(current);
    return current;
}
template<class T> Record<T> own(const T& event) {
    Record<T> stored{event, {}, {}};
    if constexpr (std::is_same_v<T, GpuSampling>) {
        stored.text = event.setup_gap ? event.setup_gap : "";
        stored.value.setup_gap = nullptr;
    } else if constexpr (std::is_same_v<T, GpuSamplingGap>) {
        stored.text = event.reason ? event.reason : "";
        stored.value.reason = nullptr;
    }
    if constexpr (requires { event.site; }) {
        const auto& site = event.site;
        const bool known = site.file || site.node_id || site.line || site.column;
        stored.source = copy_source(source_site(known ? &site : nullptr));
        stored.value.site.file = nullptr;
        stored.value.site.node_id = nullptr;
    }
    return stored;
}

template<class T>
void append_locked(Report& state, std::vector<Record<T>>& records, const T& event) {
    if (!active(state)) return;
    if (!valid(event)) {
        ++state.invalid_drops;
        return;
    }
    if constexpr (std::is_same_v<T, HostEvent>) {
        if (event.point == Point::StepBoundary &&
            (event.step != current_step() || event.step == std::numeric_limits<std::uint32_t>::max())) {
            ++state.invalid_drops;
            return;
        }
        // Logical progress is independent of optional record storage. A
        // dropped boundary must not relabel all work in the following step.
        if (event.point == Point::StepBoundary)
            detail::step.store(event.step + 1, std::memory_order_relaxed);
    }
    try {
        records.push_back(own(event));
    } catch (...) {
        state.storage_drops.fetch_add(1, std::memory_order_relaxed);
    }
}
template<class T>
void append(std::vector<Record<T>> Report::* records, const T& event) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        append_locked(state, state.*records, event);
    } catch (...) {
        state.storage_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

// Stream serialization: no whole-document allocation on the failure path.
void quoted(std::FILE* file, const std::string& text) {
    std::fputc('"', file);
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            std::fputc('\\', file);
            std::fputc(c, file);
        } else if (c < 0x20) {
            std::fprintf(file, "\\u%04x", static_cast<unsigned>(c));
        } else {
            std::fputc(c, file);
        }
    }
    std::fputc('"', file);
}
void write_source(std::FILE* file, const Source& site) {
    std::fputs(",\"site\":{\"file\":", file);
    quoted(file, site.file);
    std::fputs(",\"node_id\":", file);
    quoted(file, site.node_id);
    std::fprintf(file, ",\"line\":%llu,\"column\":%llu}", site.line, site.column);
}
void write(std::FILE* file, const Record<HostEvent>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"point\":\"%s\",\"step\":%" PRIu32
        ",\"host_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
        ",\"host_available\":%s,\"cpu_available\":%s",
        point_name(event.point), event.step, event.host_ns, event.cpu_ns,
        event.host_ns ? "true" : "false", event.cpu_ns ? "true" : "false");
    write_source(file, record.source);
    std::fputc('}', file);
}
void write(std::FILE* file, const Record<WaitInterval>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"step\":%" PRIu32 ",\"reason\":\"%s\",\"begin_ns\":%" PRIu64
        ",\"end_ns\":%" PRIu64 ",\"timing_available\":%s",
        event.step, reason_name(event.reason), event.begin_ns, event.end_ns,
        event.begin_ns ? "true" : "false");
    write_source(file, record.source);
    std::fputc('}', file);
}
void write(std::FILE* file, const Record<GpuInterval>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"serial\":%" PRIu64
        ",\"commit_step\":%" PRIu32 ",\"commit_ns\":%" PRIu64
        ",\"scheduled_begin_ns\":%" PRIu64 ",\"scheduled_end_ns\":%" PRIu64
        ",\"gpu_begin_ns\":%" PRIu64 ",\"gpu_end_ns\":%" PRIu64
        ",\"ops\":%" PRIu32 ",\"bytes\":%" PRIu64 ",\"timing_available\":%s",
        event.device, event.serial, event.commit_step, event.commit_ns,
        event.scheduled_begin_ns, event.scheduled_end_ns, event.gpu_begin_ns,
        event.gpu_end_ns, event.ops, event.bytes, event.gpu_begin_ns ? "true" : "false");
    write_source(file, record.source);
    std::fputc('}', file);
}
void write(std::FILE* file, const Record<GpuStepInterval>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"serial\":%" PRIu64
        ",\"step\":%" PRIu32 ",\"gpu_begin_ns\":%" PRIu64
        ",\"gpu_end_ns\":%" PRIu64 ",\"timing_available\":%s}",
        event.device, event.serial, event.step, event.gpu_begin_ns,
        event.gpu_end_ns, event.gpu_begin_ns ? "true" : "false");
}
void sampling_number(std::FILE* file, const char* key, std::uint64_t value) {
    std::fprintf(file, ",\"%s\":%" PRIu64, key, value);
}
void write(std::FILE* file, const Record<GpuSampling>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"serial\":%" PRIu64 ",\"setup_gap\":",
        event.device, event.serial);
    quoted(file, record.text);
    sampling_number(file, "sample_buffer_allocations", event.sample_buffer_allocations);
    sampling_number(file, "compute_encoders", event.compute_encoders);
    sampling_number(file, "blit_encoders", event.blit_encoders);
    sampling_number(file, "sampled_passes", event.sampled_passes);
    sampling_number(file, "additional_compute_encoders", event.additional_compute_encoders);
    sampling_number(file, "calibration_queries", event.calibration_queries);
    sampling_number(file, "calibration_host_ns", event.calibration_host_ns);
    sampling_number(file, "before_cpu_ns", event.before_cpu_ns);
    sampling_number(file, "before_gpu_ticks", event.before_gpu_ticks);
    sampling_number(file, "after_cpu_ns", event.after_cpu_ns);
    sampling_number(file, "after_gpu_ticks", event.after_gpu_ticks);
    sampling_number(file, "resolved_intervals", event.resolved_intervals);
    sampling_number(file, "missing_intervals", event.missing_intervals);
    std::fputc('}', file);
}
void write(std::FILE* file, const Record<GpuSamplingGap>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"serial\":%" PRIu64 ",\"step\":%" PRIu32
        ",\"compute\":%s,\"additional_encoder\":%s,\"timing_incomplete\":%s,\"reason\":",
        event.device, event.serial, event.step, event.compute ? "true" : "false",
        event.additional_encoder ? "true" : "false", event.timing_incomplete ? "true" : "false");
    quoted(file, record.text);
    std::fputc('}', file);
}
void write(std::FILE* file, const Record<WarmupInterval>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"function\":%" PRIu32
        ",\"step\":%" PRIu32 ",\"begin_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64
        ",\"timing_available\":%s", event.device, event.function, event.step,
        event.begin_ns, event.end_ns, event.begin_ns ? "true" : "false");
    write_source(file, record.source);
    std::fputc('}', file);
}
void write(std::FILE* file, const Record<CompileInterval>& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"pipeline\":%" PRIu32
        ",\"step\":%" PRIu32 ",\"timed_path\":%s,\"begin_ns\":%" PRIu64
        ",\"end_ns\":%" PRIu64 ",\"timing_available\":%s", event.device,
        event.pipeline, event.step, event.timed_path ? "true" : "false",
        event.begin_ns, event.end_ns, event.begin_ns ? "true" : "false");
    write_source(file, record.source);
    std::fputc('}', file);
}
void write(std::FILE* file, const DeviceRecord& record) {
    const auto& event = record.value;
    std::fprintf(file, "{\"device\":%" PRId32 ",\"backend\":", event.device);
    quoted(file, record.backend);
    std::fputs(",\"architecture\":", file);
    quoted(file, record.architecture);
    std::fputs(",\"device_key\":", file);
    quoted(file, record.device_key);
    std::fprintf(file, ",\"compute_units\":%" PRIu64 ",\"max_threads_per_group\":%" PRIu64
        ",\"threadgroup_memory_bytes\":%" PRIu64 ",\"simd_width\":%" PRIu64
        ",\"working_set_bytes\":%" PRIu64 ",\"unified_memory\":%s,\"driver\":",
        event.compute_units, event.max_threads_per_group, event.threadgroup_memory_bytes,
        event.simd_width, event.working_set_bytes,
        event.unified_memory_known ? (event.unified_memory ? "true" : "false") : "null");
    quoted(file, record.driver);
    std::fputs(",\"os_build\":", file);
    quoted(file, record.os_build);
    std::fputc('}', file);
}
template<class T, class Predicate>
void array(std::FILE* file, const char* name, const std::vector<T>& entries, Predicate include) {
    std::fprintf(file, ",\"%s\":[", name);
    bool first = true;
    for (const auto& entry : entries) {
        if (!include(entry)) continue;
        if (!first) std::fputc(',', file);
        first = false;
        write(file, entry);
    }
    std::fputc(']', file);
}
template<class T>
void array(std::FILE* file, const char* name, const std::vector<T>& entries) {
    array(file, name, entries, [](const T&) { return true; });
}
void write_document(std::FILE* file, const Report& state) {
    const auto storage = state.storage_drops.load(std::memory_order_relaxed);
    std::fprintf(file, "{\"schema\":1,\"clock\":\"%s\",\"clock_units\":\"ns\","
        "\"complete\":%s,\"dropped_records\":{\"invalid\":%" PRIu64 ",\"storage\":%" PRIu64 "},"
        "\"step_attribution\":\"Steps end at backward or explicit marks; optimizer work after a boundary "
        "and backward traversal belong to the next step. The first record may include initialization, and the final partial "
        "record may include optimizer work, printing and final drain.\"",
        host_clock_name(), state.invalid_drops == 0 && storage == 0 ? "true" : "false",
        state.invalid_drops, storage);
    array(file, "device_capabilities", state.devices);
    array(file, "steps", state.hosts, [](const auto& record) { return record.value.point == Point::StepBoundary; });
    array(file, "marks", state.hosts, [](const auto& record) { return record.value.point != Point::StepBoundary; });
    array(file, "waits", state.waits);
    array(file, "gpu", state.gpu);
    array(file, "gpu_steps", state.gpu_steps);
    array(file, "gpu_sampling", state.sampling);
    array(file, "gpu_sampling_gaps", state.sampling_gaps);
    const bool attribution_complete =
        std::all_of(state.sampling_gaps.begin(), state.sampling_gaps.end(), [](const auto& value) {
            return !value.value.timing_incomplete;
        }) &&
        std::all_of(state.sampling.begin(), state.sampling.end(), [](const auto& value) {
            return (value.text.empty() || value.text == "none") && value.value.missing_intervals == 0;
        });
    std::fprintf(file, ",\"gpu_attribution_complete\":%s", attribution_complete ? "true" : "false");
    std::fprintf(file, ",\"gpu_leaf_attribution_complete\":%s",
        attribution_complete && state.sampling_gaps.empty() ? "true" : "false");
    array(file, "warmups", state.warmups);
    array(file, "compiles", state.compiles);
    std::fputs("}\n", file);
}

void configure() noexcept {
    try {
        auto& state = report();
        std::lock_guard lock(state.mutex);
        if (state.configured) return;
        state.configured = true;
        const auto path = platform::environment_value("QUIDRA_TIMELINE");
        if (!path || path->empty()) return;
        state.path = *path;
        detail::time_steps.store(platform::environment_value_is("QUIDRA_COUNTERS_STEP", "marks"),
                                 std::memory_order_relaxed);
        detail::enabled.store(true, std::memory_order_relaxed);
    } catch (...) {
        // Optional observation cannot turn initialization into failure.
    }
}
struct Startup {
    Startup() noexcept { configure(); }
    ~Startup() {
        if (report().runtime_remembered) {
            note_point(Point::ExitBegin);
            note_point(Point::ExitEnd);
        }
        (void)flush_report();
    }
};
Startup startup;

} // namespace
namespace detail {

void mark_program_started_slow() noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (state.program || state.finalized || state.path.empty()) return;
        state.program = true;
        try {
            state.hosts.reserve(4096);
            state.waits.reserve(4096);
            state.gpu.reserve(4096);
            state.gpu_steps.reserve(4096);
            state.sampling.reserve(4096);
            state.sampling_gaps.reserve(4096);
            state.warmups.reserve(4096);
            state.compiles.reserve(4096);
            state.devices.reserve(16);
        } catch (...) {
            state.storage_drops.fetch_add(1, std::memory_order_relaxed);
        }
        if (state.runtime_remembered) {
            append_locked(state, state.hosts,
                HostEvent{Point::ProcessStart, current_step(), state.process_start, 0, {}});
            append_locked(state, state.hosts,
                HostEvent{Point::RuntimeInit, current_step(), state.runtime_init, state.runtime_cpu, {}});
        }
    } catch (...) {
        state.storage_drops.fetch_add(1, std::memory_order_relaxed);
    }
}
void note_point_slow(Point point, const counters::SourceSite* site) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (!active(state)) return;
        bool* once = nullptr;
        if (point == Point::FirstStatement) once = &state.first_statement;
        else if (point == Point::ExitBegin) once = &state.exit_begun;
        else if (point == Point::ExitEnd) once = &state.exit_ended;
        if (once && *once) return;
        if (once) *once = true;
        const auto host = point == Point::ProcessStart ? platform::process_start_ns() : platform::monotonic_ns();
        const auto cpu = point == Point::ProcessStart ? 0 : platform::process_cpu_ns();
        append_locked(state, state.hosts, HostEvent{point, current_step(), host, cpu,
            site ? *site : counters::SourceSite{}});
    } catch (...) {
        state.storage_drops.fetch_add(1, std::memory_order_relaxed);
    }
}
void note_host_slow(const HostEvent& event) noexcept { append(&Report::hosts, event); }
void note_wait_slow(const WaitInterval& event) noexcept { append(&Report::waits, event); }
void note_wait_slow(counters::WaitReason reason, std::uint64_t begin_ns,
                    std::uint64_t end_ns, const counters::SourceSite* site) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (!active(state)) return;
        append_locked(state, state.waits, WaitInterval{current_step(), reason, begin_ns, end_ns,
            site ? *site : counters::SourceSite{}});
    } catch (...) {
        state.storage_drops.fetch_add(1, std::memory_order_relaxed);
    }
}
void note_gpu_slow(const GpuInterval& event) noexcept { append(&Report::gpu, event); }
void note_gpu_step_slow(const GpuStepInterval& event) noexcept { append(&Report::gpu_steps, event); }
void note_gpu_sampling_slow(const GpuSampling& event) noexcept { append(&Report::sampling, event); }
void note_gpu_sampling_gap_slow(const GpuSamplingGap& event) noexcept { append(&Report::sampling_gaps, event); }
void note_warmup_slow(const WarmupInterval& event) noexcept { append(&Report::warmups, event); }
void note_compile_slow(const CompileInterval& event) noexcept { append(&Report::compiles, event); }
void note_device_slow(const DeviceCapabilities& event) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (!active(state)) return;
        try {
            DeviceRecord record{event, event.backend ? event.backend : "",
                event.architecture ? event.architecture : "", event.device_key ? event.device_key : "",
                event.driver ? event.driver : "", event.os_build ? event.os_build : ""};
            record.value.backend = record.value.architecture = record.value.device_key = nullptr;
            record.value.driver = record.value.os_build = nullptr;
            const auto existing = std::find_if(state.devices.begin(), state.devices.end(),
                [&event](const auto& entry) { return entry.value.device == event.device; });
            if (existing == state.devices.end()) state.devices.push_back(std::move(record));
            else *existing = std::move(record);
        } catch (...) {
            state.storage_drops.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        state.storage_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace detail

void remember_runtime_initialized() noexcept {
    configure();
    if (!enabled()) return;
    try {
        auto& state = report();
        std::lock_guard lock(state.mutex);
        if (state.runtime_remembered) return;
        state.runtime_remembered = true;
        state.process_start = platform::process_start_ns();
        state.runtime_init = platform::monotonic_ns();
        state.runtime_cpu = platform::process_cpu_ns();
    } catch (...) {}
}

bool flush_report() noexcept {
    if (!enabled()) return false;
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (state.finalized) return state.written;
        if (!state.program || state.path.empty()) return false;
        state.finalized = true;
        // Completion order can vary across threads. Serial/step identity and
        // timestamps retain chronology without nondeterministic JSON order.
        std::sort(state.gpu.begin(), state.gpu.end(), [](const auto& left, const auto& right) {
            return std::tie(left.value.device, left.value.serial) < std::tie(right.value.device, right.value.serial);
        });
        std::sort(state.gpu_steps.begin(), state.gpu_steps.end(), [](const auto& left, const auto& right) {
            return std::tie(left.value.device, left.value.serial, left.value.step,
                            left.value.gpu_begin_ns, left.value.gpu_end_ns) <
                   std::tie(right.value.device, right.value.serial, right.value.step,
                            right.value.gpu_begin_ns, right.value.gpu_end_ns);
        });
        std::sort(state.sampling.begin(), state.sampling.end(), [](const auto& left, const auto& right) {
            return std::tie(left.value.device, left.value.serial, left.text) <
                   std::tie(right.value.device, right.value.serial, right.text);
        });
        std::sort(state.sampling_gaps.begin(), state.sampling_gaps.end(), [](const auto& left, const auto& right) {
            return std::tie(left.value.device, left.value.serial, left.value.step, left.text,
                            left.value.compute, left.value.additional_encoder) <
                   std::tie(right.value.device, right.value.serial, right.value.step, right.text,
                            right.value.compute, right.value.additional_encoder);
        });
        std::FILE* file = nullptr;
        const bool standard_error = state.path == "-" || state.path == "stderr";
        if (standard_error) file = stderr;
#ifdef _WIN32
        else file = _fsopen(state.path.c_str(), "w", _SH_DENYNO);
#else
        else file = std::fopen(state.path.c_str(), "w");
#endif
        if (!file) return false;
        bool okay = false;
        {
            // Especially for stderr, every fragment belongs to one document;
            // another thread must not insert diagnostics between fragments.
            StreamLock stream_lock(file);
            write_document(file, state);
            okay = std::ferror(file) == 0 && std::fflush(file) == 0;
        }
        const bool closed = standard_error || std::fclose(file) == 0;
        state.written = okay && closed;
        return state.written;
    } catch (...) {
        return false;
    }
}

} // namespace quidra::timeline
