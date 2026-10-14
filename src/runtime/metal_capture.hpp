#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace quidra::profile::metal {

enum class State : std::uint8_t {
    Off, Armed, NotReached, NoMetalWork, Unsupported, Active, Stopped,
    Partial, Unavailable, Interrupted
};
enum class Reason : std::uint8_t { StepEnded, NormalExit, ImmediateFailure };
enum class Gap : std::uint8_t {
    None, UnsupportedOS, DestinationUnsupported, AlreadyCapturing, PathInvalid,
    PathUnavailable, StorageFailed, InvalidHandle, StartFailed, StopFailed,
    DeviceOmitted, HeldStream, StreamBusy, OpaqueWorkUnverified,
    SpanningBufferOmitted, InvalidCommandLifecycle, UncommittedCommand,
    ExternalStop, ImmediateFailure, NormalPartialExit, StopBusy,
    ArtifactUnverified, NativeException, Count
};
const char* name(State state) noexcept;
const char* name(Reason reason) noexcept;
const char* name(Gap gap) noexcept;
struct NativeError {
    std::string domain, message;
    std::int64_t code{};
    bool truncated{}, storage_failed{};
};
struct Status {
    State state{State::Off};
    Reason completion_reason{Reason::NormalExit};
    std::uint64_t requested_step{};
    std::int32_t selected_device{-1};
    std::string output_path;
    NativeError error;
    bool start_attempted{}, start_succeeded{}, stop_attempted{}, stopped{};
    bool artifact_exists_known{}, artifact_exists{}, exact_core_step{};
    // Device capture may contain opaque queues. Core cannot certify them.
    bool external_queues_verified{};
    bool start_clock_known{}, stop_clock_known{};
    // False means only immutable requested_step and atomic gaps are available.
    bool snapshot_complete{true};
    bool has_commands{};
    std::uint64_t first_step{}, last_step{}, first_serial{}, last_serial{};
    std::uint64_t created_commands{}, committed_commands{}, extra_commits{};
    std::uint64_t omitted_devices{}, omitted_buffers{}, held_edges{}, busy_edges{};
    std::uint64_t manager_acquires{}, manager_releases{}, manager_queries{}, start_attempts{}, stop_attempts{};
    std::uint64_t path_queries{}, clock_queries{}, metadata_drops{};
    std::uint64_t descriptors_allocated{}, native_device_retains{}, command_storage_growths{};
    std::uint64_t start_begin_ns{}, start_end_ns{}, stop_begin_ns{}, stop_end_ns{};
    std::array<std::uint64_t, static_cast<std::size_t>(Gap::Count)> gaps{};
};
struct Request {
    bool requested{};
    std::uint64_t step{};
    std::string_view output_path;
    std::span<const std::string_view> excluded_paths;
};
// Supplied after the backend has tried its existing safe commit path. The
// helper never creates/ends/commits/waits for any buffer or stream itself.
struct Edge {
    bool prior_work_closed{true}, held{}, busy{}, opaque_work{};
    std::uint64_t extra_commits{}, omitted_buffers{};
};
struct CommandInput {
    std::uint64_t step{}, serial{};
    std::int32_t device{-1};
    void* native_device{};
    Edge edge;
};
struct CommandToken {
    std::uint64_t epoch{}, step{}, serial{};
    std::int32_t device{-1};
    explicit operator bool() const noexcept { return epoch != 0; }
};
// Direct generic tests inject this metadata/manager adapter. Production uses
// the real process-wide manager. Context must outlive Capture; callbacks must
// not reenter Capture. No callback here creates a queue/buffer, dispatches or
// waits. A successful acquire is paired with exactly one release.
struct NativeApi {
    void* context{};
    bool (*available)(void*) noexcept{};
    bool (*acquire)(void*) noexcept{};
    bool (*supports_destination)(void*) noexcept{};
    bool (*is_capturing)(void*) noexcept{};
    bool (*start)(void*, void* native_device, const char* output_path, NativeError&) noexcept{};
    bool (*stop)(void*) noexcept{};
    void (*release)(void*) noexcept{};
    std::uint64_t (*clock)(void*) noexcept{};
    void (*destroy)(void*) noexcept{}; // production adapter context only
    bool (*query_succeeded)(void*) noexcept{}; // null: bool query is known
};
namespace detail { NativeApi make_native_api(Status&) noexcept; }
class Capture;
struct Creation { std::shared_ptr<Capture> capture; Status status; };
Creation make_capture(const Request&, const NativeApi* test_api = nullptr) noexcept;

class Capture {
public:
    ~Capture();
    // Call BEFORE the actual commandBuffer creation. Start captures only new
    // buffers; the token alone does not claim successful creation/commitment.
    CommandToken before_command(const CommandInput&) noexcept;
    bool command_created(CommandToken) noexcept;
    bool command_committed(CommandToken, std::uint64_t first_step,
                           std::uint64_t last_step, bool capture_extra_commit = false) noexcept;
    // Boundary n ends n. Backend commits eligible open work before this call.
    void boundary(std::uint64_t ending_step, const Edge& = {}) noexcept;
    // Metadata for actual capture-induced prior commits/unsafe edges, with no
    // premature manager start before the first eligible command creation.
    void note_edge(const Edge&) noexcept;
    // Nonblocking is mandatory for immediate-failure finalization. False
    // means ownership could not be acquired now; no foreign capture is stopped.
    bool stop(Reason, const Edge& = {}, bool nonblocking = false) noexcept;
    // Failure publication must use snapshot(true) after stop(..., true).
    // Busy fallback is allocation-free and labels all mutable fields unknown.
    Status snapshot(bool nonblocking = false) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Capture(std::unique_ptr<Impl>);
    friend Creation make_capture(const Request&, const NativeApi*) noexcept;
};

} // namespace quidra::profile::metal
