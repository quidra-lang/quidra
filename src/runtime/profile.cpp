#include "runtime/profile.hpp"
#include "runtime/capture_request.hpp"
#include "runtime/metal_capture.hpp"
#include "platform/clock.hpp"
#include "platform/environment.hpp"
#include "platform/private_directory.hpp"
#include "platform/user_cache_directory.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <share.h>
#endif

namespace quidra::profile {
namespace detail {
std::atomic<bool> enabled{false};
std::atomic<bool> split{false};
std::atomic<bool> capture_configured{false};
std::atomic<std::uint64_t> capture_step{0};
}
namespace {
using Kind = ProfileEvent::Kind;
using GpuRole = ProfileEvent::GpuRole;
struct Source { std::string file, node; unsigned long long line{}, column{}; };
struct Record {
    ProfileEvent value;
    Source source;
    std::string owner, name, kernel;
    std::uint64_t token{};
    bool pending{}, auto_closed{};
};
struct DeviceRecord {
    DeviceCapabilities value;
    std::string backend, architecture, key, driver, os;
};
struct Report {
    std::mutex mutex;
    std::string path;
#ifdef _WIN32
    std::filesystem::path profile_native_path;
#endif
    bool configured{}, program{}, finalized{}, written{}, split{};
    bool capture{};
    std::uint64_t capture_step{}, invalid{};
    bool capture_marks{}, capture_status_received{}, capture_preparation_failed{};
    std::unique_ptr<CaptureConfiguration> capture_configuration;
    std::string capture_report_path;
    std::filesystem::path capture_report_native_path;
    metal::Status capture_status;
    std::uint64_t preparation_begin{}, preparation_end{}, preparation_directories{}, preparation_sessions{};
    bool preparation_clock_known{};
    std::uint64_t reported_capture_step{}, reported_capture_storage{};
    bool reported_capture_saturated{};
    std::atomic<std::uint64_t> capture_storage{0};
    std::atomic<std::uint64_t> storage{0};
    std::vector<Record> records;
    std::vector<DeviceRecord> devices;
};
Report& report() {
    // Older exit callbacks may still enter generated runtime code after
    // Startup flushes. Keep the finalized state and immutable views valid
    // through those callbacks, without a heap allocation for the state itself.
    alignas(Report) static unsigned char storage[sizeof(Report)];
    static Report* const state = ::new (static_cast<void*>(storage)) Report;
    return *state;
}
std::atomic<bool> capture_claimed{false}, capture_finished{false}, capture_saturated{false};
std::atomic<bool> capture_report_finalized{false};
std::atomic<bool> capture_marks{false};
std::atomic<const CaptureConfiguration*> published_capture{nullptr};
std::atomic<CaptureFinalizer> capture_finalizer{nullptr};
std::atomic<std::uint64_t> next_thread{1};
thread_local std::uint64_t thread_id{}, statement_token{};
std::uint64_t thread() {
    if (!thread_id) thread_id = next_thread.fetch_add(1, std::memory_order_relaxed);
    return thread_id;
}
bool active(const Report& state) { return state.program && !state.finalized && !state.path.empty(); }
bool pair(std::uint64_t a, std::uint64_t b) { return (a == 0 && b == 0) || (a != 0 && b >= a); }
bool available(std::uint64_t a, std::uint64_t b) { return a != 0 && b >= a; }
const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::HostOp: return "host_op"; case Kind::Dispatch: return "dispatch";
    case Kind::CommandBuffer: return "command_buffer"; case Kind::Copy: return "copy";
    case Kind::Fill: return "fill"; case Kind::Alloc: return "alloc"; case Kind::Free: return "free";
    case Kind::Sync: return "sync"; case Kind::PackageRange: return "package_range";
    case Kind::PlanRun: return "plan_run"; case Kind::Replay: return "replay";
    case Kind::Statement: return "statement";
    }
    return nullptr;
}
GpuRole gpu_role(const ProfileEvent& event) {
    if (event.gpu_role != GpuRole::Automatic) return event.gpu_role;
    if (event.device < 0) return GpuRole::None;
    switch (event.kind) {
    case Kind::CommandBuffer: case Kind::PlanRun: case Kind::Replay: return GpuRole::Container;
    case Kind::Dispatch: case Kind::Copy: case Kind::Fill: return GpuRole::Activity;
    default: return available(event.gpu_begin_ns, event.gpu_end_ns) ? GpuRole::Activity : GpuRole::None;
    }
}
const char* role_name(GpuRole role) {
    switch (role) {
    case GpuRole::Automatic: return "automatic";
    case GpuRole::None: return "none";
    case GpuRole::Activity: return "activity";
    case GpuRole::Container: return "container";
    }
    return nullptr;
}
counters::SourceSite site(const counters::SourceSite* given) {
    if (given) return *given;
    counters::SourceSite result{}; (void)counters::current_source_site(result); return result;
}
Record own(const ProfileEvent& event) {
    Record result; result.value = event;
    result.value.gpu_role = gpu_role(event);
    const bool known = event.site.file || event.site.node_id || event.site.line || event.site.column;
    const auto source = site(known ? &event.site : nullptr);
    result.source = {source.file ? source.file : "", source.node_id ? source.node_id : "",
                     source.line, source.column};
    result.owner = event.owner ? event.owner : "core";
    result.name = event.name ? event.name : kind_name(event.kind);
    result.kernel = event.kernel ? event.kernel : result.name;
    result.value.owner = result.value.name = result.value.kernel = nullptr;
    result.value.site.file = result.value.site.node_id = nullptr;
    if (!result.value.host_thread) result.value.host_thread = thread();
    return result;
}
bool valid(const ProfileEvent& event) {
    const auto role = gpu_role(event);
    return kind_name(event.kind) && event.device >= -1 &&
        role_name(role) && (role == GpuRole::None || event.device >= 0) &&
        (role != GpuRole::None || event.gpu_begin_ns == 0) &&
        pair(event.host_begin_ns, event.host_end_ns) && pair(event.gpu_begin_ns, event.gpu_end_ns) &&
        (event.gpu_begin_ns == 0 || event.device >= 0);
}
Record* token_record(Report& state, std::uint64_t token) {
    if (!token || token > state.records.size()) return nullptr;
    auto& record = state.records[static_cast<std::size_t>(token - 1)];
    return record.token == token ? &record : nullptr;
}
std::uint64_t append_range(Report& state, ProfileEvent event) {
    auto record = own(event);
    record.pending = true; record.token = state.records.size() + 1;
    const auto token = record.token;
    state.records.push_back(std::move(record));
    return token;
}
void quote(std::FILE* file, const std::string& text) {
    std::fputc('"', file);
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { std::fputc('\\', file); std::fputc(c, file); }
        else if (c < 0x20) std::fprintf(file, "\\u%04x", static_cast<unsigned>(c));
        else std::fputc(c, file);
    }
    std::fputc('"', file);
}
void number(std::FILE* file, const char* name, std::uint64_t value) {
    std::fprintf(file, ",\"%s\":%llu", name, static_cast<unsigned long long>(value));
}
void boolean(std::FILE* file, const char* name, bool value, bool known = true) {
    std::fprintf(file, ",\"%s\":%s", name, known ? (value ? "true" : "false") : "null");
}
void separator(std::FILE*, bool&);
void capture_document(std::FILE* file, const Report& state) {
    const auto& status = state.capture_status;
    const bool known = status.snapshot_complete;
    const auto drops = state.reported_capture_storage;
    std::fprintf(file, "{\"requested_step\":%llu,\"state\":\"%s\",\"completion_reason\":",
        static_cast<unsigned long long>(state.capture_step), metal::name(status.state));
    if (known) std::fprintf(file, "\"%s\"", metal::name(status.completion_reason));
    else std::fputs("null", file);
    std::fputs(",\"output_path\":", file);
    quote(file, state.capture_configuration ? state.capture_configuration->output_path : status.output_path);
    std::fputs(",\"report_path\":", file); quote(file, state.capture_report_path);
    number(file, "current_step", state.reported_capture_step);
    boolean(file, "step_saturated", state.reported_capture_saturated);
    boolean(file, "snapshot_complete", known && drops == 0);
    boolean(file, "retained_snapshot_complete", known);
    number(file, "dropped_status_updates", drops);
    if (known && status.selected_device >= 0) std::fprintf(file, ",\"selected_device\":%d", status.selected_device);
    else std::fputs(",\"selected_device\":null", file);
#define CAPTURE_BOOL(field) boolean(file, #field, status.field, known)
    CAPTURE_BOOL(start_attempted); CAPTURE_BOOL(start_succeeded); CAPTURE_BOOL(stop_attempted);
    CAPTURE_BOOL(stopped); CAPTURE_BOOL(artifact_exists_known);
    boolean(file, "artifact_exists", status.artifact_exists, known && status.artifact_exists_known);
    boolean(file, "exact_core_step", status.exact_core_step && drops == 0, known);
    CAPTURE_BOOL(external_queues_verified); CAPTURE_BOOL(start_clock_known); CAPTURE_BOOL(stop_clock_known);
    CAPTURE_BOOL(has_commands);
#undef CAPTURE_BOOL
    const auto value = [&](const char* name, std::uint64_t amount) {
        if (known) number(file, name, amount);
        else std::fprintf(file, ",\"%s\":null", name);
    };
#define CAPTURE_NUMBER(field) value(#field, status.field)
    CAPTURE_NUMBER(first_step); CAPTURE_NUMBER(last_step); CAPTURE_NUMBER(first_serial); CAPTURE_NUMBER(last_serial);
    CAPTURE_NUMBER(created_commands); CAPTURE_NUMBER(committed_commands); CAPTURE_NUMBER(extra_commits);
    CAPTURE_NUMBER(omitted_devices); CAPTURE_NUMBER(omitted_buffers); CAPTURE_NUMBER(held_edges); CAPTURE_NUMBER(busy_edges);
    CAPTURE_NUMBER(manager_acquires); CAPTURE_NUMBER(manager_releases); CAPTURE_NUMBER(manager_queries);
    CAPTURE_NUMBER(start_attempts); CAPTURE_NUMBER(stop_attempts); CAPTURE_NUMBER(path_queries); CAPTURE_NUMBER(clock_queries);
    CAPTURE_NUMBER(metadata_drops); CAPTURE_NUMBER(descriptors_allocated); CAPTURE_NUMBER(native_device_retains);
    CAPTURE_NUMBER(command_storage_growths);
#undef CAPTURE_NUMBER
    const auto timing = [&](const char* name, std::uint64_t amount, bool clock_known) {
        if (known && clock_known) number(file, name, amount);
        else std::fprintf(file, ",\"%s\":null", name);
    };
    timing("start_begin_ns", status.start_begin_ns, status.start_clock_known);
    timing("start_end_ns", status.start_end_ns, status.start_clock_known);
    timing("stop_begin_ns", status.stop_begin_ns, status.stop_clock_known);
    timing("stop_end_ns", status.stop_end_ns, status.stop_clock_known);
    std::fputs(",\"error\":", file);
    if (known) {
        std::fputs("{\"domain\":", file); quote(file, status.error.domain);
        std::fputs(",\"message\":", file); quote(file, status.error.message);
        std::fprintf(file, ",\"code\":%lld", static_cast<long long>(status.error.code));
        boolean(file, "truncated", status.error.truncated); boolean(file, "storage_failed", status.error.storage_failed);
        std::fputc('}', file);
    } else std::fputs("null", file);
    std::fputs(",\"gaps\":{", file);
    bool first = true;
    for (std::size_t i = 1; i < status.gaps.size(); ++i) {
        if (!status.gaps[i]) continue;
        separator(file, first);
        std::fprintf(file, "\"%s\":%llu", metal::name(static_cast<metal::Gap>(i)),
                     static_cast<unsigned long long>(status.gaps[i]));
    }
    std::fputs("},\"preparation\":{\"clock_queries\":2", file);
    number(file, "directory_attempts", state.preparation_directories);
    number(file, "session_attempts", state.preparation_sessions);
    boolean(file, "failed", state.capture_preparation_failed);
    boolean(file, "clock_known", state.preparation_clock_known);
    if (state.preparation_clock_known) {
        number(file, "begin_ns", state.preparation_begin); number(file, "end_ns", state.preparation_end);
        number(file, "host_ns", state.preparation_end - state.preparation_begin);
    } else std::fputs(",\"begin_ns\":null,\"end_ns\":null,\"host_ns\":null", file);
    std::fputs("}}", file);
}
void micros(std::FILE* file, std::uint64_t ns) {
    std::fprintf(file, "%llu.%03llu", static_cast<unsigned long long>(ns / 1000),
                 static_cast<unsigned long long>(ns % 1000));
}
void source(std::FILE* file, const Source& value) {
    std::fputs("\"site\":{\"file\":", file); quote(file, value.file);
    std::fputs(",\"node_id\":", file); quote(file, value.node);
    std::fprintf(file, ",\"line\":%llu,\"column\":%llu}", value.line, value.column);
}
void separator(std::FILE* file, bool& first) {
    if (!first) std::fputc(',', file);
    first = false;
}
void trace(std::FILE* file, const Record& record, bool gpu, bool& first) {
    const auto& e = record.value;
    const auto begin = gpu ? e.gpu_begin_ns : e.host_begin_ns;
    const auto end = gpu ? e.gpu_end_ns : e.host_end_ns;
    if (record.pending || !available(begin, end)) return;
    separator(file, first);
    std::fputs("{\"name\":", file); quote(file, record.name);
    std::fprintf(file, ",\"cat\":\"%s\",\"ph\":\"X\",\"pid\":%llu,\"tid\":%llu,\"ts\":",
        gpu ? "gpu" : kind_name(e.kind),
        static_cast<unsigned long long>(gpu ? static_cast<std::uint64_t>(e.device) + 2 : 1),
        static_cast<unsigned long long>(gpu ? static_cast<std::uint64_t>(e.stream) + 1 : e.host_thread));
    micros(file, begin); std::fputs(",\"dur\":", file); micros(file, end - begin);
    std::fputs(",\"args\":{", file); source(file, record.source);
    std::fputs(",\"owner\":", file); quote(file, record.owner);
    std::fputs(",\"kernel\":", file); quote(file, record.kernel);
    std::fprintf(file, ",\"gpu_role\":\"%s\"", role_name(e.gpu_role));
    number(file, "bytes", e.bytes); number(file, "kernel_hash", e.kernel_hash);
    number(file, "host_begin_ns", e.host_begin_ns); number(file, "host_end_ns", e.host_end_ns);
    number(file, "gpu_begin_ns", e.gpu_begin_ns); number(file, "gpu_end_ns", e.gpu_end_ns);
    std::fprintf(file, ",\"host_available\":%s,\"gpu_available\":%s,\"auto_closed\":%s}}",
        available(e.host_begin_ns, e.host_end_ns) ? "true" : "false",
        available(e.gpu_begin_ns, e.gpu_end_ns) ? "true" : "false", record.auto_closed ? "true" : "false");
}
auto key(const Record& record) { return std::tie(record.source.node, record.owner, record.kernel); }
void add(std::uint64_t& value, std::uint64_t amount, bool& overflow) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
        value = std::numeric_limits<std::uint64_t>::max(); overflow = true;
    } else value += amount;
}
struct GpuMetric {
    std::uint64_t calls{}, known{}, ns{}, bytes{};
    bool overflow{};
    void append(const Record& record) {
        ++calls;
        if (!record.pending && available(record.value.gpu_begin_ns, record.value.gpu_end_ns)) {
            ++known; add(ns, record.value.gpu_end_ns - record.value.gpu_begin_ns, overflow);
        }
        add(bytes, record.value.bytes, overflow);
    }
};
void metric(std::FILE* file, const char* prefix, const GpuMetric& value) {
    std::fprintf(file, ",\"%s_calls\":%llu,\"%s_gpu_known_calls\":%llu,\"%s_gpu_unknown_calls\":%llu,"
        "\"%s_gpu_ns\":%llu,\"%s_bytes\":%llu,\"%s_overflow\":%s",
        prefix, static_cast<unsigned long long>(value.calls), prefix, static_cast<unsigned long long>(value.known),
        prefix, static_cast<unsigned long long>(value.calls - value.known),
        prefix, static_cast<unsigned long long>(value.ns), prefix, static_cast<unsigned long long>(value.bytes),
        prefix, value.overflow ? "true" : "false");
}
void document(std::FILE* file, const Report& state) {
    const auto dropped = state.storage.load(std::memory_order_relaxed);
    std::uint64_t open = 0;
    for (const auto& r : state.records) if (r.pending) ++open;
    std::fprintf(file, "{\"schema\":2,\"clock_units\":\"ns\",\"displayTimeUnit\":\"ns\","
        "\"gpu_aggregation\":\"typed_activity_container\",\"bytes_scope\":\"inclusive_events\",\"complete\":%s,"
        "\"dropped_records\":{\"invalid\":%llu,\"storage\":%llu},\"open_ranges\":%llu,"
        "\"configuration\":{\"split_requested\":%s,\"capture_step\":",
        state.invalid == 0 && dropped == 0 && open == 0 ? "true" : "false",
        static_cast<unsigned long long>(state.invalid), static_cast<unsigned long long>(dropped),
        static_cast<unsigned long long>(open), state.split ? "true" : "false");
    if (state.capture) std::fprintf(file, "%llu", static_cast<unsigned long long>(state.capture_step));
    else std::fputs("null", file);
    std::fputs(",\"device_instrumentation\":\"producer-supplied; timing and capture availability reported explicitly\"},"
        "\"device_capabilities\":[", file);
    bool first = true;
    for (const auto& d : state.devices) {
        separator(file, first);
        std::fprintf(file, "{\"device\":%d,\"backend\":", d.value.device); quote(file, d.backend);
        std::fputs(",\"architecture\":", file); quote(file, d.architecture);
        std::fputs(",\"device_key\":", file); quote(file, d.key);
        number(file, "compute_units", d.value.compute_units);
        number(file, "max_threads_per_group", d.value.max_threads_per_group);
        number(file, "threadgroup_memory_bytes", d.value.threadgroup_memory_bytes);
        number(file, "simd_width", d.value.simd_width); number(file, "working_set_bytes", d.value.working_set_bytes);
        std::fprintf(file, ",\"unified_memory\":%s,\"driver\":",
            d.value.unified_memory_known ? (d.value.unified_memory ? "true" : "false") : "null");
        quote(file, d.driver); std::fputs(",\"os_build\":", file); quote(file, d.os); std::fputc('}', file);
    }
    std::fputs("],\"traceEvents\":[", file); first = true;
    for (std::size_t i = 0; i < state.records.size(); ++i) {
        const auto& r = state.records[i];
        bool host_seen = false;
        bool gpu_seen = !available(r.value.gpu_begin_ns, r.value.gpu_end_ns);
        for (std::size_t j = 0; j < i && (!host_seen || !gpu_seen); ++j) {
            const auto& previous = state.records[j];
            host_seen |= previous.value.host_thread == r.value.host_thread;
            gpu_seen |= previous.value.device == r.value.device && previous.value.stream == r.value.stream &&
                available(previous.value.gpu_begin_ns, previous.value.gpu_end_ns);
        }
        if (!host_seen) {
            separator(file, first);
            std::fprintf(file, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%llu,"
                "\"args\":{\"name\":\"host %llu\"}}", static_cast<unsigned long long>(r.value.host_thread),
                static_cast<unsigned long long>(r.value.host_thread));
        }
        if (!gpu_seen && available(r.value.gpu_begin_ns, r.value.gpu_end_ns)) {
            separator(file, first);
            std::fprintf(file, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":%llu,\"tid\":%llu,"
                "\"args\":{\"name\":\"gpu %d stream %u\"}}", static_cast<unsigned long long>(r.value.device) + 2,
                static_cast<unsigned long long>(r.value.stream) + 1, r.value.device, r.value.stream);
        }
        trace(file, r, false, first); trace(file, r, true, first);
    }
    std::fputs("],\"aggregate\":[", file); first = true;
    for (std::size_t i = 0; i < state.records.size();) {
        const auto& r = state.records[i];
        std::uint64_t calls = 0, host = 0, bytes = 0, hc = 0;
        GpuMetric activity, container;
        bool overflow = false;
        std::size_t j = i;
        for (; j < state.records.size() && key(state.records[j]) == key(r); ++j) {
            const auto& e = state.records[j]; ++calls;
            if (!e.pending && available(e.value.host_begin_ns, e.value.host_end_ns)) {
                ++hc; add(host, e.value.host_end_ns - e.value.host_begin_ns, overflow);
            }
            if (e.value.gpu_role == GpuRole::Activity) activity.append(e);
            else if (e.value.gpu_role == GpuRole::Container) container.append(e);
            add(bytes, e.value.bytes, overflow);
        }
        separator(file, first); std::fputs("{\"node_id\":", file); quote(file, r.source.node);
        std::fputs(",\"owner\":", file); quote(file, r.owner);
        std::fputs(",\"kernel\":", file); quote(file, r.kernel);
        overflow |= activity.overflow || container.overflow;
        number(file, "calls", calls); number(file, "host_calls", hc); number(file, "gpu_calls", activity.known);
        number(file, "host_unknown_calls", calls - hc); number(file, "gpu_unknown_calls", activity.calls - activity.known);
        number(file, "host_ns", host); number(file, "gpu_ns", activity.ns); number(file, "bytes", bytes);
        metric(file, "activity", activity); metric(file, "container", container);
        std::fputs(",\"host_us\":", file); micros(file, host);
        std::fputs(",\"gpu_us\":", file); micros(file, activity.ns);
        std::fprintf(file, ",\"overflow\":%s,", overflow ? "true" : "false"); source(file, r.source);
        std::fputc('}', file); i = j;
    }
    std::fputs("],\"capture\":", file);
    if (state.capture) capture_document(file, state);
    else std::fputs("null", file);
    std::fputs("}\n", file);
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
void preparation_failed(Report& state, metal::Gap gap) noexcept {
    state.capture_preparation_failed = true;
    state.capture_status.state = metal::State::Unavailable;
    ++state.capture_status.gaps[static_cast<std::size_t>(gap)];
}
void prepare_capture(Report& state) noexcept {
    namespace fs = std::filesystem;
    state.capture_status.requested_step = state.capture_step;
    state.capture_status.state = metal::State::Armed;
    state.preparation_begin = platform::monotonic_ns();
    try {
        auto configuration = std::make_unique<CaptureConfiguration>();
        configuration->requested_step = state.capture_step;
        const auto cache = platform::user_cache_directory();
        if (!cache) preparation_failed(state, metal::Gap::PathUnavailable);
        else {
            ++state.preparation_directories;
            const auto root = platform::open_private_root(*cache);
            if (!root.path) preparation_failed(state, metal::Gap::PathUnavailable);
            else {
                ++state.preparation_directories;
                const auto profiles = platform::open_private_subdirectory(*root.path, "profiles");
                if (!profiles.path) preparation_failed(state, metal::Gap::PathUnavailable);
                else {
                    std::random_device random;
                    std::optional<fs::path> session;
                    for (unsigned attempt = 0; attempt < 16 && !session; ++attempt) {
                        ++state.preparation_sessions;
                        const auto nonce = (static_cast<std::uint64_t>(random()) << 32) ^ random();
                        const auto candidate = *profiles.path / ("capture-" + std::to_string(nonce));
                        std::error_code error;
                        // An existing session is never reused, even for this user.
                        if (!fs::create_directory(candidate, error)) {
                            if (error) break;
                            continue;
                        }
#ifndef _WIN32
                        // The exclusive create inherits umask; tighten the new
                        // session before publishing any path below it.
                        fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, error);
                        if (error) break;
#endif
                        ++state.preparation_directories;
                        const auto checked = platform::open_private_subdirectory(*profiles.path, candidate.filename());
                        if (checked.path) session = *checked.path;
                        else break;
                    }
                    if (!session) preparation_failed(state, metal::Gap::PathUnavailable);
                    else {
                        configuration->output_path = utf8(*session / "capture.gputrace");
                        state.capture_report_native_path = *session / "capture.json";
                        state.capture_report_path = utf8(state.capture_report_native_path);
                        configuration->excluded_storage.reserve(5);
                        if (!state.path.empty()) configuration->excluded_storage.push_back(state.path);
                        for (const auto* name : {"QUIDRA_TIMELINE", "QUIDRA_COUNTERS"}) {
                            if (const auto path = platform::environment_value(name); path && !path->empty())
                                configuration->excluded_storage.push_back(*path);
                        }
                        counters::SourceSite source{};
                        if (counters::current_source_site(source) && source.file && *source.file)
                            configuration->excluded_storage.emplace_back(source.file);
                        configuration->excluded_storage.push_back(state.capture_report_path);
                        configuration->excluded_paths.reserve(configuration->excluded_storage.size());
                        for (const auto& path : configuration->excluded_storage)
                            configuration->excluded_paths.emplace_back(path);
                        state.capture_status.output_path = configuration->output_path;
                        state.capture_configuration = std::move(configuration);
                        published_capture.store(state.capture_configuration.get(), std::memory_order_release);
                    }
                }
            }
        }
    } catch (...) {
        preparation_failed(state, metal::Gap::StorageFailed);
        state.capture_storage.fetch_add(1, std::memory_order_relaxed);
    }
    state.preparation_end = platform::monotonic_ns();
    state.preparation_clock_known = available(state.preparation_begin, state.preparation_end);
}
void configure() noexcept {
        try {
            auto& state = report();
            std::lock_guard lock(state.mutex);
            if (state.configured) return;
            state.configured = true;
            if (const auto request = platform::environment_value("QUIDRA_METAL_CAPTURE")) {
                if (const auto step = parse_capture_step(*request)) {
                    state.capture = true; state.capture_step = *step;
                    state.capture_marks = platform::environment_value_is("QUIDRA_COUNTERS_STEP", "marks");
                    capture_marks.store(state.capture_marks, std::memory_order_relaxed);
                    detail::capture_configured.store(true, std::memory_order_relaxed);
                }
            }
#ifdef _WIN32
            // Preserve the native Unicode path before any heap-pressure flush.
            // UTF-8 is kept separately for JSON and exclusion metadata.
            const auto wide_path = platform::environment_wide_value(L"QUIDRA_PROFILE");
            if (!wide_path || wide_path->empty()) return;
            state.profile_native_path = std::filesystem::path(*wide_path);
            state.path = utf8(state.profile_native_path);
#else
            const auto path = platform::environment_value("QUIDRA_PROFILE");
            if (!path || path->empty()) return;
            state.path = *path;
#endif
            state.split = platform::environment_value_is("QUIDRA_PROFILE_SPLIT", "1");
            detail::split.store(state.split, std::memory_order_relaxed);
            detail::enabled.store(true, std::memory_order_relaxed);
        } catch (...) {}
}
struct Startup {
    Startup() noexcept { configure(); }
    ~Startup() {
        if (capture_requested() && !capture_finished.load(std::memory_order_relaxed))
            if (const auto finish = capture_finalizer.load(std::memory_order_acquire)) finish(false);
        (void)flush_report();
    }
} startup;
} // namespace
namespace detail {
std::uint64_t current_thread_slow() noexcept { return thread(); }
void mark_program_started_slow() noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (state.program || state.finalized || (state.path.empty() && !state.capture)) return;
        state.program = true;
        if (state.capture) {
            prepare_capture(state);
            capture_claimed.store(true, std::memory_order_release);
        }
        if (!state.path.empty()) { state.records.reserve(4096); state.devices.reserve(16); }
    } catch (...) { state.storage.fetch_add(1, std::memory_order_relaxed); }
}
void record_slow(const ProfileEvent& event) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex); if (!active(state)) return;
        if (!valid(event)) { ++state.invalid; return; }
        state.records.push_back(own(event));
    } catch (...) { state.storage.fetch_add(1, std::memory_order_relaxed); }
}
std::uint64_t range_begin_slow(const char* owner, const char* name, std::int32_t device,
                              Kind kind, const counters::SourceSite* given) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex); if (!active(state)) return 0;
        if (!kind_name(kind) || device < -1) { ++state.invalid; return 0; }
        ProfileEvent event; event.kind = kind; event.device = device;
        event.owner = owner; event.name = name; event.site = site(given);
        event.host_begin_ns = platform::monotonic_ns();
        return append_range(state, event);
    } catch (...) { state.storage.fetch_add(1, std::memory_order_relaxed); return 0; }
}
void range_end_slow(std::uint64_t token) noexcept {
    if (!token) return;
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex); if (!active(state)) return;
        auto* r = token_record(state, token);
        if (!r || !r->pending || r->value.host_thread != thread()) { ++state.invalid; return; }
        r->value.host_end_ns = platform::monotonic_ns(); r->pending = false;
    } catch (...) { state.storage.fetch_add(1, std::memory_order_relaxed); }
}
void statement_changed_slow(const counters::SourceSite* given) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex); if (!active(state)) return;
        auto* previous = token_record(state, statement_token);
        if (previous && previous->pending && given &&
            previous->source.file == (given->file ? given->file : "") &&
            previous->source.node == (given->node_id ? given->node_id : "") &&
            previous->source.line == given->line && previous->source.column == given->column) return;
        const auto now = platform::monotonic_ns();
        if (previous && previous->pending) { previous->value.host_end_ns = now; previous->pending = false; }
        statement_token = 0;
        if (!given) return;
        ProfileEvent event; event.kind = Kind::Statement; event.site = *given;
        event.owner = "quidra"; event.name = "statement"; event.host_begin_ns = now;
        statement_token = append_range(state, event);
    } catch (...) { state.storage.fetch_add(1, std::memory_order_relaxed); }
}
void note_device_slow(const DeviceCapabilities& event) noexcept {
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex); if (!active(state)) return;
        DeviceRecord record{event, event.backend ? event.backend : "", event.architecture ? event.architecture : "",
            event.device_key ? event.device_key : "", event.driver ? event.driver : "", event.os_build ? event.os_build : ""};
        record.value.backend = record.value.architecture = record.value.device_key = nullptr;
        record.value.driver = record.value.os_build = nullptr;
        const auto found = std::find_if(state.devices.begin(), state.devices.end(),
            [&event](const auto& d) { return d.value.device == event.device; });
        if (found == state.devices.end()) state.devices.push_back(std::move(record));
        else *found = std::move(record);
    } catch (...) { state.storage.fetch_add(1, std::memory_order_relaxed); }
}
} // namespace detail

void configure_runtime() noexcept { configure(); }
bool capture_requested() noexcept { return capture_claimed.load(std::memory_order_relaxed); }
const CaptureConfiguration* capture_configuration() noexcept {
    if (!capture_requested()) return nullptr;
    return published_capture.load(std::memory_order_acquire);
}
std::uint64_t capture_current_step() noexcept {
    return capture_requested() ? detail::capture_step.load(std::memory_order_relaxed) : 0;
}
void advance_capture_step() noexcept {
    if (!capture_requested()) return;
    auto previous = detail::capture_step.load(std::memory_order_relaxed);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    while (previous != maximum && !detail::capture_step.compare_exchange_weak(previous, previous + 1,
            std::memory_order_relaxed, std::memory_order_relaxed)) {}
    if (previous == maximum) capture_saturated.store(true, std::memory_order_relaxed);
}
bool capture_time_marks_are_steps() noexcept {
    return capture_requested() && capture_marks.load(std::memory_order_relaxed);
}
void set_capture_finalizer(CaptureFinalizer finish) noexcept {
    capture_finalizer.store(finish, std::memory_order_release);
}
void note_capture(const metal::Status& value) noexcept {
    if (!capture_requested() || capture_report_finalized.load(std::memory_order_relaxed)) return;
    auto& state = report();
    try {
        std::unique_lock lock(state.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            if (!capture_report_finalized.load(std::memory_order_relaxed))
                state.capture_storage.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (state.finalized) return;
        if (value.requested_step != state.capture_step ||
            value.state > metal::State::Interrupted || value.completion_reason > metal::Reason::ImmediateFailure) {
            state.capture_storage.fetch_add(1, std::memory_order_relaxed); return;
        }
        try {
            auto owned = value;
            state.capture_status = std::move(owned);
            state.capture_status_received = true;
        } catch (...) { state.capture_storage.fetch_add(1, std::memory_order_relaxed); }
    } catch (...) {
        if (!capture_report_finalized.load(std::memory_order_relaxed))
            state.capture_storage.fetch_add(1, std::memory_order_relaxed);
    }
}
namespace {
bool write_report(const Report& state, bool capture) {
    const bool standard = !capture && (state.path == "-" || state.path == "stderr");
    std::FILE* file = nullptr;
    if (standard) file = stderr;
#ifdef _WIN32
    else if (capture) file = _wfsopen(state.capture_report_native_path.c_str(), L"w", _SH_DENYNO);
    else if (!state.profile_native_path.empty())
        file = _wfsopen(state.profile_native_path.c_str(), L"w", _SH_DENYNO);
    else file = _fsopen(state.path.c_str(), "w", _SH_DENYNO);
    if (file) _lock_file(file);
#else
    else file = std::fopen(capture ? state.capture_report_path.c_str() : state.path.c_str(), "w");
    if (file) flockfile(file);
#endif
    if (!file) return false;
    if (capture) {
        std::fputs("{\"schema\":1,\"kind\":\"metal_capture\",\"clock_units\":\"ns\",\"capture\":", file);
        capture_document(file, state); std::fputs("}\n", file);
    } else document(file, state);
    const bool okay = std::ferror(file) == 0 && std::fflush(file) == 0;
#ifdef _WIN32
    _unlock_file(file);
#else
    funlockfile(file);
#endif
    const bool closed = standard || std::fclose(file) == 0;
    return okay && closed;
}
}
bool flush_report() noexcept {
    if (!enabled() && !capture_requested()) return false;
    auto& state = report();
    try {
        std::lock_guard lock(state.mutex);
        if (state.finalized) return state.written;
        if (!state.program) return false;
        state.finalized = true;
        capture_report_finalized.store(true, std::memory_order_relaxed);
        capture_finished.store(true, std::memory_order_relaxed);
        state.reported_capture_step = detail::capture_step.load(std::memory_order_relaxed);
        state.reported_capture_saturated = state.reported_capture_step == std::numeric_limits<std::uint64_t>::max() &&
            capture_saturated.load(std::memory_order_relaxed);
        state.reported_capture_storage = state.capture_storage.load(std::memory_order_relaxed);
        if (state.capture && !state.capture_status_received && !state.capture_preparation_failed)
            state.capture_status.state = state.reported_capture_step < state.capture_step
                ? metal::State::NotReached : metal::State::NoMetalWork;
        if (!state.path.empty()) {
            const auto now = platform::monotonic_ns();
            for (auto& r : state.records) if (r.pending && r.value.kind == Kind::Statement) {
                r.value.host_end_ns = now; r.pending = false; r.auto_closed = true;
            }
            std::sort(state.records.begin(), state.records.end(), [](const auto& a, const auto& b) { return key(a) < key(b); });
            std::sort(state.devices.begin(), state.devices.end(), [](const auto& a, const auto& b) { return a.value.device < b.value.device; });
        }
        bool okay = true;
        if (!state.capture_report_path.empty()) okay = write_report(state, true);
        else if (state.capture) okay = false;
        if (!state.path.empty()) {
            const bool profile_okay = write_report(state, false);
            okay = profile_okay && okay;
        }
        state.written = okay; return okay;
    } catch (...) { return false; }
}
} // namespace quidra::profile
