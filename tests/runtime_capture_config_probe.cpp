#include "runtime/profile.hpp"
#include "runtime/metal_capture.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
namespace {
std::atomic<unsigned> queries{};
std::atomic<bool> reject{};
std::string source_file = "capture-source.qui";
bool check_late_configuration{};
bool expected_late_marks{};
void older_exit_callback() {
    if (!check_late_configuration) return;
    const auto* config = quidra::profile::capture_configuration();
    if (!config || config->requested_step != 0 || config->output_path.empty()) std::_Exit(12);
    for (std::size_t i = 0; i < config->excluded_paths.size(); ++i)
        if (config->excluded_paths[i] != config->excluded_storage[i]) std::_Exit(12);
    if (quidra::profile::capture_time_marks_are_steps() != expected_late_marks ||
        !quidra::profile::flush_report() || !quidra::profile::flush_report()) std::_Exit(13);
    quidra::profile::mark_program_started();
    quidra::profile::statement_changed(nullptr);
    quidra::profile::note_capture({});
}
// Registered before the profile translation unit constructs its Startup.
struct OlderExitRegistration {
    OlderExitRegistration() { std::atexit(older_exit_callback); }
} older_exit_registration;
void finalize(bool immediate) noexcept {
    if (immediate) std::_Exit(8);
    const auto* config = quidra::profile::capture_configuration();
    if (!config) std::_Exit(9);
    quidra::profile::metal::Status status;
    status.requested_step = config->requested_step;
    status.state = quidra::profile::metal::State::NoMetalWork;
    status.extra_commits = 1;
    quidra::profile::note_capture(status);
}
void forbidden_finalize(bool) noexcept { std::_Exit(7); }
}
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
bool current_source_site(SourceSite& value) {
    ++queries; value = {source_file.c_str(), "capture-source", 1, 1}; return true;
}
}
int main(int argc, char** argv) {
    using namespace quidra::profile;
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (capture_requested() || capture_configuration() || capture_time_marks_are_steps()) return 3;
    advance_capture_step();
    if (capture_current_step() != 0) return 3;
    if (mode == "--compiler" || mode == "--off") {
        configure_runtime(); record({}); statement_changed(nullptr); range_end(range_begin("test", "off"));
        note_capture({}); set_capture_finalizer(forbidden_finalize);
        return queries == 0 && !capture_requested() ? 0 : 3;
    }
    if (mode == "--prepare-pressure") reject = true;
    mark_program_started();
    reject = false;
    const auto* config = capture_configuration();
    if (mode == "--invalid") return !capture_requested() && !config && queries == 0 ? 0 : 3;
    if (!capture_requested()) return 3;
    if (mode == "--bad-cache" || mode == "--prepare-pressure")
        return !config ? 0 : 4;
    if (!config || config->output_path.empty() || config->excluded_paths.size() != config->excluded_storage.size()) return 4;
    for (std::size_t i = 0; i < config->excluded_paths.size(); ++i)
        if (config->excluded_paths[i].data() != config->excluded_storage[i].data() ||
            config->excluded_paths[i] != config->excluded_storage[i]) return 4;
    source_file = "mutated-source.qui";
    bool source_owned = false;
    for (const auto path : config->excluded_paths) if (path == "capture-source.qui") source_owned = true;
    if (!source_owned) return 4;
    if (mode == "--late-exit") {
        check_late_configuration = true; expected_late_marks = capture_time_marks_are_steps(); return 0;
    }
    if (mode == "--concurrent-flush") {
        std::thread worker([] {
            if (std::getc(stdin) != '\n') std::_Exit(11);
            for (unsigned i = 0; i < 100; ++i) advance_capture_step();
            note_capture({});
            std::puts("advanced"); std::fflush(stdout);
        });
        const bool okay = flush_report(); worker.join();
        return okay ? 0 : 6;
    }
    if (mode == "--finalizer") { set_capture_finalizer(finalize); return 0; }
    if (mode == "--saturation") {
        detail::capture_step.store(std::numeric_limits<std::uint64_t>::max() - 1, std::memory_order_relaxed);
        advance_capture_step(); advance_capture_step();
        if (capture_current_step() != std::numeric_limits<std::uint64_t>::max()) return 5;
    } else if (mode == "--threads") {
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 8; ++i) threads.emplace_back([] { for (unsigned j = 0; j < 1000; ++j) advance_capture_step(); });
        for (auto& worker : threads) worker.join();
        if (capture_current_step() != 8000) return 5;
    } else if (mode == "--marks") {
        if (!capture_time_marks_are_steps()) return 5;
        advance_capture_step();
    } else if (capture_time_marks_are_steps()) return 5;
    if (mode == "--owned" || mode == "--pressure" || mode == "--snapshot" || mode == "--delayed") {
        metal::Status status;
        status.requested_step = config->requested_step;
        status.output_path = config->output_path;
        status.state = metal::State::Stopped;
        status.completion_reason = metal::Reason::StepEnded;
        status.selected_device = 4;
        status.start_attempted = status.start_succeeded = status.stop_attempted = status.stopped = true;
        status.artifact_exists_known = status.artifact_exists = status.exact_core_step = true;
        status.start_clock_known = status.stop_clock_known = true;
        status.has_commands = true;
        status.first_step = status.last_step = config->requested_step;
        status.first_serial = status.last_serial = std::numeric_limits<std::uint64_t>::max();
        status.created_commands = status.committed_commands = 2;
        status.manager_acquires = status.manager_releases = 1;
        status.start_begin_ns = 100; status.start_end_ns = 110;
        status.stop_begin_ns = 200; status.stop_end_ns = 210;
        status.error.domain = "owned-domain";
        status.error.message.assign(100, 'm'); status.error.code = -7;
        status.gaps[static_cast<std::size_t>(metal::Gap::OpaqueWorkUnverified)] = 3;
        note_capture(status);
        if (mode == "--pressure") {
            status.state = metal::State::Interrupted; status.error.message.assign(100, 'x');
            status.exact_core_step = false;
            status.gaps[static_cast<std::size_t>(metal::Gap::InvalidCommandLifecycle)] = 1;
            reject = true; note_capture(status); reject = false;
        } else if (mode == "--delayed") {
            status.state = metal::State::Partial; status.exact_core_step = false;
            status.gaps[static_cast<std::size_t>(metal::Gap::InvalidCommandLifecycle)] = 1;
            note_capture(status);
            advance_capture_step();
            if (capture_current_step() != 1) return 5;
        } else if (mode == "--snapshot") {
            metal::Status unavailable; unavailable.requested_step = config->requested_step;
            unavailable.state = metal::State::Unavailable; unavailable.snapshot_complete = false;
            unavailable.gaps[static_cast<std::size_t>(metal::Gap::StopBusy)] = 1;
            note_capture(unavailable);
        }
        status.output_path.clear(); status.error.domain = "mutated"; status.error.message = "mutated";
    }
    if (mode == "--failure") {
        metal::Status status; status.requested_step = config->requested_step;
        status.state = metal::State::Interrupted; status.completion_reason = metal::Reason::ImmediateFailure;
        status.gaps[static_cast<std::size_t>(metal::Gap::ImmediateFailure)] = 1; note_capture(status);
        reject = true; (void)flush_report(); std::_Exit(101);
    }
    set_capture_finalizer(forbidden_finalize);
    reject = true;
    const bool okay = flush_report() && flush_report();
    const auto before = capture_current_step();
    // Late producer and logical boundary do not mutate the finalized report.
    note_capture({}); advance_capture_step();
    if (before != std::numeric_limits<std::uint64_t>::max() && capture_current_step() != before + 1) return 5;
    reject = false;
    return okay ? 0 : 6;
}
