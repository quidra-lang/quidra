#include "runtime/metal_capture.hpp"
#include "platform/clock.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace quidra::profile::metal {
namespace {
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
void add(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    value = amount > maximum - value ? maximum : value + amount;
}
void atomic_add(std::atomic<std::uint64_t>& value) noexcept {
    auto previous = value.load(std::memory_order_relaxed);
    while (previous != maximum && !value.compare_exchange_weak(previous, previous + 1,
            std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
void gap(Status& status, Gap value, std::uint64_t count = 1) noexcept {
    if (value != Gap::None) add(status.gaps[static_cast<std::size_t>(value)], count);
}
bool valid_utf8(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned remaining = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; code = first & 0x07; minimum = 0x10000; }
        else return false;
        if (remaining > text.size() - i) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
void limit_error_text(std::string& text, NativeError& error) noexcept {
    constexpr std::size_t limit = 512;
    if (text.size() > limit) {
        std::size_t cut = limit;
        while (cut != 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
        text.resize(cut); error.truncated = true;
    }
    if (!text.empty() && !valid_utf8(text)) { text.clear(); error.storage_failed = true; }
}
Gap prepare_path(const Request& request, Status& status) {
    namespace fs = std::filesystem;
    if (!valid_utf8(request.output_path)) return Gap::PathInvalid;
    const auto path = utf8_path(request.output_path).lexically_normal();
    if (!path.is_absolute() || path.extension() != ".gputrace") return Gap::PathInvalid;
    std::error_code error;
    add(status.path_queries);
    auto parent = fs::canonical(path.parent_path(), error);
    if (error) return Gap::PathUnavailable;
    add(status.path_queries);
    if (!fs::is_directory(parent, error) || error) return Gap::PathUnavailable;
    const auto normalized = parent / path.filename();
    // /dev paths are streams/devices, never a persistent capture destination.
    if (normalized == "/dev" || normalized.string().starts_with("/dev/")) return Gap::PathInvalid;
    add(status.path_queries);
    const auto destination = fs::symlink_status(normalized, error);
    if (error && error != std::errc::no_such_file_or_directory) return Gap::PathUnavailable;
    if (destination.type() != fs::file_type::not_found) return Gap::PathInvalid;
    for (const auto excluded : request.excluded_paths) {
        if (excluded.empty() || excluded == "-" || excluded == "stderr" || excluded == "stdout") continue;
        if (!valid_utf8(excluded)) return Gap::PathInvalid;
        add(status.path_queries);
        const auto other = fs::weakly_canonical(utf8_path(excluded), error);
        if (error) return Gap::PathUnavailable;
        if (other == normalized) return Gap::PathInvalid;
    }
    const auto utf8 = normalized.u8string();
    const std::string_view normalized_utf8(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    if (!valid_utf8(normalized_utf8)) return Gap::PathInvalid;
    status.output_path.assign(normalized_utf8);
    return Gap::None;
}
} // namespace

const char* name(State state) noexcept {
    switch (state) {
    case State::Off: return "off";
    case State::Armed: return "armed";
    case State::NotReached: return "not_reached";
    case State::NoMetalWork: return "no_metal_work";
    case State::Unsupported: return "unsupported";
    case State::Active: return "active";
    case State::Stopped: return "stopped";
    case State::Partial: return "partial";
    case State::Unavailable: return "unavailable";
    case State::Interrupted: return "interrupted";
    }
    return "unknown";
}
const char* name(Reason reason) noexcept {
    switch (reason) {
    case Reason::StepEnded: return "step_ended";
    case Reason::NormalExit: return "normal_partial_exit";
    case Reason::ImmediateFailure: return "immediate_failure";
    }
    return "unknown";
}
const char* name(Gap value) noexcept {
    switch (value) {
    case Gap::None: return "none";
    case Gap::UnsupportedOS: return "unsupported_os";
    case Gap::DestinationUnsupported: return "destination_unsupported";
    case Gap::AlreadyCapturing: return "already_capturing";
    case Gap::PathInvalid: return "path_invalid";
    case Gap::PathUnavailable: return "path_unavailable";
    case Gap::StorageFailed: return "storage_failed";
    case Gap::InvalidHandle: return "invalid_handle";
    case Gap::StartFailed: return "start_failed";
    case Gap::StopFailed: return "stop_failed";
    case Gap::DeviceOmitted: return "device_omitted";
    case Gap::HeldStream: return "held_stream";
    case Gap::StreamBusy: return "stream_busy";
    case Gap::OpaqueWorkUnverified: return "opaque_work_unverified";
    case Gap::SpanningBufferOmitted: return "spanning_buffer_omitted";
    case Gap::InvalidCommandLifecycle: return "invalid_command_lifecycle";
    case Gap::UncommittedCommand: return "uncommitted_command";
    case Gap::ExternalStop: return "external_stop";
    case Gap::ImmediateFailure: return "immediate_failure";
    case Gap::NormalPartialExit: return "normal_partial_exit";
    case Gap::StopBusy: return "stop_busy";
    case Gap::ArtifactUnverified: return "artifact_unverified";
    case Gap::NativeException: return "native_exception";
    case Gap::Count: break;
    }
    return "unknown";
}

#ifndef __APPLE__
namespace detail {
namespace {
bool unavailable(void*) noexcept { return false; }
bool no_start(void*, void*, const char*, NativeError&) noexcept { return false; }
void no_release(void*) noexcept {}
std::uint64_t native_clock(void*) noexcept {
    try { return platform::monotonic_ns(); } catch (...) { return 0; }
}
}
NativeApi make_native_api(Status&) noexcept {
    return {nullptr, unavailable, unavailable, unavailable, unavailable,
            no_start, unavailable, no_release, native_clock, nullptr};
}
} // namespace detail
#endif

struct Capture::Impl {
    struct Command { CommandToken token; bool committed{}; };
    mutable std::mutex mutex;
    std::atomic<std::uint64_t> busy_stops{};
    Status status;
    NativeApi api;
    std::vector<Command> commands;
    std::vector<std::int32_t> omitted_device_indices;
    void* selected_native_device{};
    bool acquired{}, owned{}, saw_metal{}, coverage_gap{}, finished{}, api_owned{}, query_known{true};
    std::uint64_t epoch{};
    ~Impl() {
        release_native();
        if (api_owned && api.destroy) api.destroy(api.context);
    }
    void release_native() noexcept {
        if (!acquired) return;
        acquired = false; add(status.manager_releases); api.release(api.context);
    }
    void note(Gap value, bool affects_core = true, std::uint64_t count = 1) noexcept {
        gap(status, value, count);
        if (affects_core) {
            coverage_gap = true; status.exact_core_step = false;
            if (status.state == State::Stopped) status.state = State::Partial;
        }
    }
    bool edge(const Edge& value) noexcept {
        add(status.extra_commits, value.extra_commits);
        add(status.omitted_buffers, value.omitted_buffers);
        if (value.omitted_buffers) note(Gap::SpanningBufferOmitted, true, value.omitted_buffers);
        if (value.opaque_work) note(Gap::OpaqueWorkUnverified, false);
        if (value.held) { add(status.held_edges); note(Gap::HeldStream); }
        if (value.busy) { add(status.busy_edges); note(Gap::StreamBusy); }
        if (!value.prior_work_closed) note(Gap::SpanningBufferOmitted);
        return value.prior_work_closed && !value.held && !value.busy && value.omitted_buffers == 0;
    }
    bool destination_fresh() noexcept {
        try {
            std::error_code error; add(status.path_queries);
            const auto destination = std::filesystem::symlink_status(utf8_path(status.output_path), error);
            if (error && error != std::errc::no_such_file_or_directory) { note(Gap::PathUnavailable); return false; }
            if (destination.type() != std::filesystem::file_type::not_found) { note(Gap::PathInvalid); return false; }
            return true;
        } catch (...) { note(Gap::StorageFailed); add(status.metadata_drops); return false; }
    }
    void omit_device(std::int32_t device) noexcept {
        note(Gap::DeviceOmitted);
        if (std::find(omitted_device_indices.begin(), omitted_device_indices.end(), device) != omitted_device_indices.end()) return;
        try { omitted_device_indices.push_back(device); add(status.omitted_devices); }
        catch (...) { note(Gap::StorageFailed); add(status.metadata_drops); }
    }
    std::uint64_t now() noexcept {
        if (!api.clock) return 0;
        add(status.clock_queries); return api.clock(api.context);
    }
    bool capturing() noexcept {
        add(status.manager_queries);
        const bool result = api.is_capturing(api.context);
        query_known = !api.query_succeeded || api.query_succeeded(api.context);
        return result;
    }
    bool stop_locked(Reason reason, const Edge& value) noexcept {
        if (finished) { release_native(); return true; }
        const bool safe = edge(value);
        status.completion_reason = reason;
        if (!owned) {
            if (status.state == State::Armed) status.state = saw_metal ? State::NotReached : State::NoMetalWork;
            finished = true; release_native(); return true;
        }
        if (reason == Reason::NormalExit) note(Gap::NormalPartialExit);
        if (reason == Reason::ImmediateFailure) note(Gap::ImmediateFailure);
        const bool active = capturing();
        if (!query_known) {
            owned = false; finished = true; status.state = State::Unavailable;
            note(Gap::NativeException); release_native(); return true;
        }
        if (!active) {
            owned = false; finished = true; status.state = State::Interrupted;
            note(Gap::ExternalStop); release_native(); return true;
        }
        const auto missing = status.created_commands - status.committed_commands;
        if (missing) { note(Gap::UncommittedCommand, true, missing); add(status.omitted_buffers, missing); }
        status.exact_core_step = reason == Reason::StepEnded && safe && !coverage_gap &&
            busy_stops.load(std::memory_order_relaxed) == 0 && status.created_commands != 0 && missing == 0;
        status.stop_attempted = true; add(status.stop_attempts);
        status.stop_begin_ns = now();
        const bool stopped = api.stop(api.context);
        status.stop_end_ns = now();
        status.stop_clock_known = status.stop_begin_ns != 0 && status.stop_end_ns >= status.stop_begin_ns;
        owned = false; finished = true; status.stopped = stopped;
        if (!stopped) { note(Gap::StopFailed); status.exact_core_step = false; }
        status.state = stopped && status.exact_core_step ? State::Stopped : State::Partial;
        if (stopped) {
            try {
                std::error_code error;
                add(status.path_queries);
                status.artifact_exists = std::filesystem::exists(utf8_path(status.output_path), error);
                status.artifact_exists_known = !error;
                if (error || !status.artifact_exists) note(Gap::ArtifactUnverified, false);
            } catch (...) { note(Gap::StorageFailed, false); add(status.metadata_drops); }
        }
        release_native(); return true;
    }

};

Capture::Capture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Capture::~Capture() { stop(Reason::NormalExit, {}, true); }
Creation make_capture(const Request& request, const NativeApi* test_api) noexcept {
    Creation result;
    if (!request.requested) return result;
    result.status.state = State::Unavailable; result.status.requested_step = request.step;
    try {
        const auto path_gap = prepare_path(request, result.status);
        if (path_gap != Gap::None) { gap(result.status, path_gap); return result; }
        auto impl = std::make_unique<Capture::Impl>();
        impl->status = std::move(result.status); impl->status.state = State::Armed;
        if (test_api) impl->api = *test_api;
        else { impl->api = detail::make_native_api(impl->status); impl->api_owned = true; }
        const auto& api = impl->api;
        if (!api.available || !api.acquire || !api.supports_destination || !api.is_capturing || !api.start || !api.stop || !api.release) {
            impl->status.state = State::Unavailable; gap(impl->status, test_api ? Gap::InvalidHandle : Gap::StorageFailed);
            result.status = std::move(impl->status); return result;
        }
        result.capture = std::shared_ptr<Capture>(new Capture(std::move(impl)));
        result.status = result.capture->snapshot();
    } catch (...) {
        result.capture.reset(); result.status.state = State::Unavailable;
        gap(result.status, Gap::StorageFailed); add(result.status.metadata_drops);
    }
    return result;
}
CommandToken Capture::before_command(const CommandInput& input) noexcept {
    try {
        std::lock_guard lock(impl_->mutex); auto& self = *impl_; auto& status = self.status;
        self.saw_metal = true;
        if (self.finished || input.step != status.requested_step) {
            if (self.owned && input.step != status.requested_step) self.note(Gap::SpanningBufferOmitted);
            return {};
        }
        if (!self.edge(input.edge)) { if (!self.owned) { status.state = State::Unavailable; self.finished = true; } return {}; }
        if (input.device < 0 || !input.native_device) { self.note(Gap::InvalidHandle); return {}; }
        if (self.owned) {
            if (input.device != status.selected_device) { self.omit_device(input.device); return {}; }
            if (input.native_device != self.selected_native_device) { self.note(Gap::InvalidHandle); add(status.omitted_buffers); return {}; }
        } else {
            if (!self.destination_fresh()) { status.state = State::Unavailable; self.finished = true; return {}; }
            if (!self.api.available(self.api.context)) { self.note(Gap::UnsupportedOS); status.state = State::Unsupported; self.finished = true; return {}; }
            add(status.manager_acquires);
            if (!self.api.acquire(self.api.context)) { self.note(Gap::NativeException); status.state = State::Unavailable; self.finished = true; return {}; }
            self.acquired = true;
            add(status.manager_queries);
            const bool supported = self.api.supports_destination(self.api.context);
            if (self.api.query_succeeded && !self.api.query_succeeded(self.api.context)) { self.note(Gap::NativeException); status.state = State::Unavailable; self.finished = true; return {}; }
            if (!supported) { self.note(Gap::DestinationUnsupported); status.state = State::Unsupported; self.finished = true; return {}; }
            const bool foreign = self.capturing();
            if (!self.query_known) { self.note(Gap::NativeException); status.state = State::Unavailable; self.finished = true; return {}; }
            if (foreign) { self.note(Gap::AlreadyCapturing); status.state = State::Unavailable; self.finished = true; return {}; }
            status.start_attempted = true; add(status.start_attempts);
            status.start_begin_ns = self.now();
            const bool started = self.api.start(self.api.context, input.native_device, status.output_path.c_str(), status.error);
            status.start_end_ns = self.now();
            status.start_clock_known = status.start_begin_ns != 0 && status.start_end_ns >= status.start_begin_ns;
            limit_error_text(status.error.domain, status.error); limit_error_text(status.error.message, status.error);
            if (status.error.storage_failed) { self.note(Gap::StorageFailed, false); add(status.metadata_drops); }
            if (!started) { self.note(Gap::StartFailed); status.state = State::Unavailable; self.finished = true; return {}; }
            self.owned = true; self.epoch = 1; status.start_succeeded = true;
            status.selected_device = input.device; self.selected_native_device = input.native_device; status.state = State::Active;
        }
        return {self.epoch, input.step, input.serial, input.device};
    } catch (...) { return {}; }
}
bool Capture::command_created(CommandToken token) noexcept {
    try {
        std::lock_guard lock(impl_->mutex); auto& self = *impl_; auto& status = self.status;
        if (!token || !self.owned || self.finished || token.epoch != self.epoch ||
            token.device != status.selected_device || token.step != status.requested_step) { self.note(Gap::InvalidCommandLifecycle); return false; }
        if (std::any_of(self.commands.begin(), self.commands.end(), [&](const auto& command) { return command.token.serial == token.serial; })) {
            self.note(Gap::InvalidCommandLifecycle); return false;
        }
        try {
            const auto capacity = self.commands.capacity();
            self.commands.push_back({token, false});
            if (self.commands.capacity() != capacity) add(status.command_storage_growths);
        }
        catch (...) { self.note(Gap::StorageFailed); add(status.metadata_drops); add(status.omitted_buffers); return false; }
        if (!status.has_commands) { status.first_serial = status.last_serial = token.serial; status.first_step = status.last_step = token.step; status.has_commands = true; }
        else { status.first_serial = std::min(status.first_serial, token.serial); status.last_serial = std::max(status.last_serial, token.serial); }
        add(status.created_commands); return true;
    } catch (...) { return false; }
}
bool Capture::command_committed(CommandToken token, std::uint64_t first, std::uint64_t last, bool extra) noexcept {
    try {
        std::lock_guard lock(impl_->mutex); auto& self = *impl_; auto& status = self.status;
        auto found = std::find_if(self.commands.begin(), self.commands.end(), [&](const auto& command) {
            return command.token.epoch == token.epoch && command.token.serial == token.serial && command.token.device == token.device && command.token.step == token.step;
        });
        if (!self.owned || self.finished || !token || found == self.commands.end() || found->committed || first > last) {
            self.note(Gap::InvalidCommandLifecycle); return false;
        }
        found->committed = true; add(status.committed_commands); if (extra) add(status.extra_commits);
        status.first_step = std::min(status.first_step, first); status.last_step = std::max(status.last_step, last);
        if (first != status.requested_step || last != status.requested_step) self.note(Gap::SpanningBufferOmitted);
        return true;
    } catch (...) { return false; }
}
void Capture::boundary(std::uint64_t step, const Edge& edge) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (step == impl_->status.requested_step) impl_->stop_locked(Reason::StepEnded, edge);
        else if (step > impl_->status.requested_step && impl_->owned) {
            impl_->note(Gap::SpanningBufferOmitted); impl_->stop_locked(Reason::StepEnded, edge);
        }
    } catch (...) {}
}
void Capture::note_edge(const Edge& edge) noexcept {
    try { std::lock_guard lock(impl_->mutex); impl_->edge(edge); } catch (...) {}
}
bool Capture::stop(Reason reason, const Edge& edge, bool nonblocking) noexcept {
    try {
        std::unique_lock lock(impl_->mutex, std::defer_lock);
        if (nonblocking) {
            if (!lock.try_lock()) { atomic_add(impl_->busy_stops); return false; }
        } else lock.lock();
        return impl_->stop_locked(reason, edge);
    } catch (...) { atomic_add(impl_->busy_stops); return false; }
}
Status Capture::snapshot(bool nonblocking) const noexcept {
    Status result;
    try {
        std::unique_lock lock(impl_->mutex, std::defer_lock);
        if (nonblocking) {
            if (!lock.try_lock()) {
                result.state = State::Unavailable; result.snapshot_complete = false;
                result.requested_step = impl_->status.requested_step;
                gap(result, Gap::StopBusy, std::max<std::uint64_t>(1, impl_->busy_stops.load(std::memory_order_relaxed)));
                return result;
            }
        } else lock.lock();
        result = impl_->status;
        const auto busy = impl_->busy_stops.load(std::memory_order_relaxed);
        if (busy) {
            gap(result, Gap::StopBusy, busy); result.exact_core_step = false;
            if (result.state == State::Stopped) result.state = State::Partial;
        }
    } catch (...) {
        result.state = State::Unavailable; result.snapshot_complete = false;
        result.requested_step = impl_->status.requested_step;
        gap(result, Gap::StorageFailed); add(result.metadata_drops);
    }
    return result;
}

} // namespace quidra::profile::metal
