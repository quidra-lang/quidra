#include "runtime_counters.hpp"
#include "platform/environment.hpp"
#include "runtime_parallel.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/native_extension.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <share.h>
#endif

namespace quidra::counters {
namespace {

constexpr auto count = static_cast<std::size_t>(Id::Count);

constexpr std::array<const char*, count> names{
    "core_dispatches",
    "allocations",
    "allocation_bytes",
    "uploads",
    "upload_bytes",
    "readbacks",
    "readback_bytes",
    "fills",
    "fill_bytes",
    "device_copies",
    "device_copy_bytes",
    "validation_slots",
    "synchronizations",
    "metal_command_buffers",
    "metal_marker_buffers",
    "metal_compute_encoders",
    "metal_blit_encoders",
    "metal_new_buffers",
    "metal_pool_hits",
    "metal_direct_uploads",
    "metal_staged_uploads",
    "metal_direct_fills",
    "metal_kernel_fills",
    "metal_pipeline_compiles",
    "host_waits",
    "host_waits_sync",
    "host_waits_read",
    "host_waits_status",
    "package_queue_borrows",
    "package_borrow_flushes",
    "package_encode_holds",
    "package_encode_scopes",
    "package_deferred_checks",
    "package_refusals",
    "package_completion_waits",
    "package_scope_violations",
    "package_warmups",
};

std::array<std::atomic<std::uint64_t>, count>& values() {
    static auto* storage = new std::array<std::atomic<std::uint64_t>, count>{};
    return *storage;
}

std::atomic<SourceSiteProvider> provider{nullptr};
std::atomic<SourceSiteProvider> user_provider{nullptr};
std::atomic<UnlocatedFailureReporter> unlocated_reporter{nullptr};

// Per-step attribution maps and the report file. Everything here is touched
// only while counting is enabled, under one mutex.
struct Report {
    std::mutex mutex;
    std::string path;
    // Only the generated program writes the report. The compiler CLI links
    // the device layer too (and `quidra run` shares the environment with the
    // program it starts), so it must not touch the report file.
    bool program{};
    std::FILE* file{};
    bool step_on_backward{true};
    std::uint64_t step{};
    std::array<std::uint64_t, count> last{};
    std::map<std::string, std::uint64_t> wait_sites;
    std::map<std::string, std::uint64_t> refusals;
    std::map<std::string, std::uint64_t> total_wait_sites;
    std::map<std::string, std::uint64_t> total_refusals;
};

Report& report() {
    // A heap singleton becomes unreachable when the JIT runtime DSO unloads.
    // Use static storage so the report is destroyed at library shutdown.
    static Report state;
    return state;
}

void append_escaped(std::string& out, const std::string& text) {
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buffer;
        } else {
            out += c;
        }
    }
}

void append_map(std::string& out, const char* key,
                const std::map<std::string, std::uint64_t>& map) {
    out += ",\"";
    out += key;
    out += "\":{";
    bool first = true;
    for (const auto& [site, amount] : map) {
        if (!first) out += ',';
        first = false;
        out += '"';
        append_escaped(out, site);
        out += "\":";
        out += std::to_string(amount);
    }
    out += '}';
}

std::FILE* report_file_locked(Report& state) {
    if (state.file) return state.file;
    if (state.path.empty() || !state.program) return nullptr;
    if (state.path == "-" || state.path == "stderr") {
        state.file = stderr;
        return state.file;
    }
#ifdef _WIN32
    // The Windows runtime deprecates fopen (C4996, an error under /WX). Its
    // fopen shares the file with _SH_DENYNO, and so does this; fopen_s would
    // share it in the stricter _SH_SECURE mode.
    state.file = _fsopen(state.path.c_str(), "w", _SH_DENYNO);
#else
    state.file = std::fopen(state.path.c_str(), "w");
#endif
    return state.file;
}

// Writes one JSON line: deltas since the previous record (or totals).
void write_record_locked(Report& state, const char* record, const char* event,
                         const char* label, bool totals) {
    std::FILE* file = report_file_locked(state);
    std::array<std::uint64_t, count> now{};
    for (std::size_t i = 0; i < count; ++i)
        now[i] = values()[i].load(std::memory_order_relaxed);
    if (file) {
        std::string out = "{\"record\":\"";
        out += record;
        out += '"';
        if (!totals) {
            out += ",\"step\":" + std::to_string(state.step);
            out += ",\"event\":\"";
            out += event;
            out += '"';
            if (label && *label) {
                out += ",\"label\":\"";
                append_escaped(out, label);
                out += '"';
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            out += ",\"";
            out += names[i];
            out += "\":";
            out += std::to_string(totals ? now[i] : now[i] - state.last[i]);
        }
        append_map(out, "wait_sites",
                   totals ? state.total_wait_sites : state.wait_sites);
        append_map(out, "refusals",
                   totals ? state.total_refusals : state.refusals);
        out += "}\n";
        std::fputs(out.c_str(), file);
        std::fflush(file);
    }
    if (!totals) {
        state.last = now;
        state.wait_sites.clear();
        state.refusals.clear();
        ++state.step;
    }
}

// Reads QUIDRA_COUNTERS once at load. The destructor writes the last step and
// the process totals; the device runtime's own exit work (deferred
// validation, teardown waits) runs earlier and is therefore included.
struct Startup {
    Startup() {
        const auto path = platform::environment_value("QUIDRA_COUNTERS");
        if (!path || path->empty()) return;
        auto& state = report();
        state.path = *path;
        if (const auto step = platform::environment_value("QUIDRA_COUNTERS_STEP"))
            state.step_on_backward = *step != "none";
        detail::enabled.store(true, std::memory_order_relaxed);
    }
    ~Startup() {
        if (!enabled()) return;
        auto& state = report();
        std::lock_guard lock(state.mutex);
        if (state.path.empty() || !state.program) return;
        write_record_locked(state, "step", "exit", nullptr, false);
        write_record_locked(state, "total", nullptr, nullptr, true);
        if (state.file && state.file != stderr) std::fclose(state.file);
        state.file = nullptr;
    }
};

} // namespace

namespace detail {
std::atomic<bool> enabled{false};

void add_slow(Id id, std::uint64_t amount) {
    const auto index = static_cast<std::size_t>(id);
    if (index < count)
        values()[index].fetch_add(amount, std::memory_order_relaxed);
}
} // namespace detail

namespace {
Startup startup;
}

const char* name(Id id) {
    const auto index = static_cast<std::size_t>(id);
    return index < count ? names[index] : "unknown";
}

void note_wait(WaitReason reason) {
    if (!enabled()) return;
    detail::add_slow(Id::HostWaits, 1);
    switch (reason) {
        case WaitReason::Sync: detail::add_slow(Id::HostWaitsSync, 1); break;
        case WaitReason::Read: detail::add_slow(Id::HostWaitsRead, 1); break;
        case WaitReason::Status: detail::add_slow(Id::HostWaitsStatus, 1); break;
    }
    SourceSite site;
    std::string key = reason == WaitReason::Sync ? "sync"
                    : reason == WaitReason::Read ? "read" : "status";
    if (current_source_site(site)) {
        key += '@';
        key += format_source_site(site);
    }
    auto& state = report();
    std::lock_guard lock(state.mutex);
    ++state.wait_sites[key];
    ++state.total_wait_sites[key];
}

void note_refusal(const char* owner, const char* function, int status) {
    if (!enabled()) return;
    detail::add_slow(Id::PackageRefusals, 1);
    std::string key = owner ? owner : "?";
    key += '/';
    key += function ? function : "?";
    key += ':';
    key += std::to_string(status);
    auto& state = report();
    std::lock_guard lock(state.mutex);
    ++state.refusals[key];
    ++state.total_refusals[key];
}

void step_boundary(StepEvent event, const char* label) {
    if (!enabled()) return;
    auto& state = report();
    std::lock_guard lock(state.mutex);
    if (event == StepEvent::Backward && !state.step_on_backward) return;
    write_record_locked(
        state, "step", event == StepEvent::Backward ? "backward" : "mark",
        label, false);
}

void set_enabled(bool value) {
    detail::enabled.store(value, std::memory_order_relaxed);
}

std::uint64_t value(Id id) {
    const auto index = static_cast<std::size_t>(id);
    return index < count ? values()[index].load(std::memory_order_relaxed) : 0;
}

std::vector<std::uint64_t> snapshot() {
    std::vector<std::uint64_t> result(count);
    for (std::size_t i = 0; i < count; ++i)
        result[i] = values()[i].load(std::memory_order_relaxed);
    return result;
}

void reset() {
    for (auto& entry : values()) entry.store(0, std::memory_order_relaxed);
    auto& state = report();
    std::lock_guard lock(state.mutex);
    state.last = {};
    state.wait_sites.clear();
    state.refusals.clear();
    state.total_wait_sites.clear();
    state.total_refusals.clear();
}

void set_source_site_provider(SourceSiteProvider value) {
    provider.store(value, std::memory_order_release);
    // The provider comes from the program runtime, so this process is a
    // Quidra program and owns the QUIDRA_COUNTERS report.
    auto& state = report();
    std::lock_guard lock(state.mutex);
    state.program = value != nullptr;
}

bool current_source_site(SourceSite& site) {
    const auto current = provider.load(std::memory_order_acquire);
    return current && current(site);
}

void set_user_source_site_provider(SourceSiteProvider value) {
    user_provider.store(value, std::memory_order_release);
}

bool current_user_source_site(SourceSite& site) {
    const auto current = user_provider.load(std::memory_order_acquire);
    return current && current(site);
}

void set_unlocated_failure_reporter(UnlocatedFailureReporter reporter) {
    unlocated_reporter.store(reporter, std::memory_order_release);
}

void report_unlocated_failure(abi::FailureReason reason, std::string_view message,
                              bool immediate_exit) {
    if (const auto reporter = unlocated_reporter.load(std::memory_order_acquire))
        reporter(reason, message, immediate_exit);
    std::fflush(stdout);
    const auto code = abi::spelling(abi::failure_reason_info(reason).code);
    std::fprintf(stderr, "Quidra runtime error[%.*s]: %.*s\n", static_cast<int>(code.size()),
                 code.data(), static_cast<int>(message.size()), message.data());
    std::fflush(nullptr);
    if (immediate_exit) std::_Exit(abi::failure_exit_status);
    std::exit(abi::failure_exit_status);
}

std::string format_source_site(const SourceSite& site) {
    std::string out;
    if (site.file && *site.file) {
        out += site.file;
        out += ':';
    }
    out += std::to_string(site.line);
    out += ':';
    out += std::to_string(site.column);
    return out;
}

} // namespace quidra::counters

extern "C" void qcore_counter_note_refusal(
    const char* owner, const char* function, int status) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    quidra::counters::note_refusal(owner, function, status);
}

extern "C" void qcore_counters_mark(const char* label) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    quidra::counters::step_boundary(quidra::counters::StepEvent::Mark, label);
}

extern "C" uint64_t qcore_counter_value(const char* name) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!name || !quidra::counters::enabled()) return 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(quidra::counters::Id::Count); ++i) {
        const auto id = static_cast<quidra::counters::Id>(i);
        if (std::strcmp(quidra::counters::name(id), name) == 0)
            return quidra::counters::value(id);
    }
    return 0;
}
