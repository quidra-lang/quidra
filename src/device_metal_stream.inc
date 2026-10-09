// Metal command stream. Included inside the anonymous namespace
// of device_compute.inc.
//
// Each Metal device has one open command buffer. Core dispatches, fills and
// copies, and package kernels that use Core's encode ABI, all append to it.
// The open buffer is committed (never waited on) when it holds enough work,
// when the GPU went idle, and at every flush point:
//   - host reads of device bytes and device synchronization (commit + wait);
//   - a native handle (buffer or queue) lent to package code outside a
//     package hold (below), because such code may commit its own command
//     buffer on Core's queue, which must run after the work already encoded
//     here.
// Command buffers on one queue run in commit order, so committing the open
// buffer (or an empty marker after package commits) and waiting for it covers
// every earlier submission, including command buffers packages committed
// themselves. Every committed buffer carries a serial; its completion
// handler advances completed_serial, so concurrent waiters never return
// before the work they observed as submitted has finished.
//
// Package completion callbacks run on a Core-owned serial queue, never in
// Metal's completion handlers, which only retire serials and hand callbacks
// over (see MetalStream::completion_queue).
//
// Core cannot see when package code that received a native handle commits
// its own command buffer: that may happen after another thread's
// synchronization. Once a device has lent a handle, every synchronization
// therefore commits a fresh command buffer (the open one or an empty marker)
// and waits for it, which covers every package commit that happened before
// the synchronization started.
//
// Package encodes (native_extension.h) follow one ownership protocol. A
// package thread holds the stream (MetalHold) from qcore_device_encode_begin,
// or from the scope open, to qcore_metal_note_work, and keeps `mutex` locked
// for that whole time, so under the lock a hold always belongs to the
// calling thread. While it is Preparing, Core works on its own encoders as
// usual (copy-on-write detaches, uploads, status page clears) but commits
// nothing, so every handle lent in the hold is used by the command buffer
// the package's work goes into. Once the package holds Core's encoder or
// command buffer (the encoder scope), Core does no work on the stream at
// all: a Metal encoder cannot save and restore its pipeline state and
// bindings, and ending it would leave the package with a released encoder.
// A request for such work is refused and recorded as a protocol violation;
// qcore_metal_note_work then discards the open command buffer and the
// runtime stops the program (GPU_SCOPE). On a thread beside which other
// threads would run on during exit (a warm-up, a completion callback, a
// task.all task) the buffer is never committed instead, the stream stays
// held, and the process ends at once. A hold never commits or waits, and it
// covers one device. A hold still open when the package code that began it
// returns is ended the same way where control comes back to Core
// (end_left_package_hold): at the next Quidra statement of that thread, at
// the end of its task.all task, after a completion callback or warm-up, and
// in the runtime's failure paths and exit.

struct MetalCompletion {
    void (*function)(void*){};
    void* context{};
};

struct MetalInFlight {
    std::uint64_t serial{};
    id<MTLCommandBuffer> command{nil}; // retained until completion
    bool done{};
    std::vector<MetalCompletion> completions;
};

// Commit thresholds. MLX starts from 40 operations / 40 MiB. Long chains of
// small element-wise Core operations cost less per operation when a command
// buffer holds 100 of them, so the operation limit is 100; operations of
// 1 MiB still reach the byte limit, and commit, every 40 operations.
struct MetalStreamConfig {
    bool stream{true};
    std::uint32_t max_ops{100};
    std::uint64_t max_bytes{40ull << 20};
    std::uint32_t idle_ops{8};
    std::uint64_t idle_ns{50000};
};

std::uint64_t metal_env_u64(const char* name, std::uint64_t fallback) {
    const auto text = platform::environment_value(name);
    if (!text || text->empty()) return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text->c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<std::uint64_t>(value) : fallback;
}

const MetalStreamConfig& metal_stream_config() {
    static const MetalStreamConfig config = [] {
        MetalStreamConfig value;
        if (const auto stream = platform::environment_value("QUIDRA_METAL_STREAM"))
            value.stream = *stream != "0";
        value.max_ops = static_cast<std::uint32_t>(std::max<std::uint64_t>(
            1, metal_env_u64("QUIDRA_METAL_MAX_OPS", value.max_ops)));
        value.max_bytes = std::max<std::uint64_t>(
            1, metal_env_u64("QUIDRA_METAL_MAX_BYTES", value.max_bytes));
        value.idle_ops = static_cast<std::uint32_t>(
            metal_env_u64("QUIDRA_METAL_IDLE_OPS", value.idle_ops));
        value.idle_ns = metal_env_u64("QUIDRA_METAL_IDLE_NS", value.idle_ns);
        // Without the stream every operation commits its own command buffer.
        if (!value.stream) value.max_ops = 1;
        return value;
    }();
    return config;
}

std::uint64_t metal_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A package thread's hold on a stream (see the protocol above).
enum class MetalHold : std::uint8_t {
    None,
    Preparing,     // stream held; Core works and lends but never commits
    Encoder,       // the package holds Core's compute encoder: no Core work
    CommandBuffer, // the package holds Core's open command buffer: no Core work
};

struct MetalStream {
    // Encoding state. Recursive: a package hold keeps it across the
    // package's own calls back into Core on the same thread.
    std::recursive_mutex mutex;
    int global_index{-1};
    int backend_index{-1};
    id<MTLDevice> device{nil};        // retained for the process lifetime
    id<MTLCommandQueue> queue{nil};   // retained for the process lifetime
    id<MTLCommandBuffer> open{nil};   // retained, not yet committed
    id<MTLComputeCommandEncoder> encoder{nil}; // Core's encoder on `open`
    std::uint32_t ops{};              // dispatches/copies/fills in `open`
    std::uint64_t bytes{};            // bytes touched by them (estimate)
    std::uint64_t opened_ns{};
    // Serial `open` receives at commit. Written under `mutex`; read without
    // it when a buffer is released (see metal_pool_give).
    std::atomic<std::uint64_t> next_serial{1};
    // Package code received a native handle outside a package hold at least
    // once, so it may commit command buffers Core does not see (sticky).
    bool package_lent{};
    // Changes whenever work is encoded into the stream (Core work, a package
    // scope's work) or a native handle is lent, before any of it is
    // committed. The unified-memory views (device_unified.inc) compare it
    // across a window to know whether device work may reference a block.
    std::atomic<std::uint64_t> work_epoch{0};
    // Highest serial a deferred status check waits for; a synchronization
    // commits a marker when nothing is open and this serial is not submitted.
    std::uint64_t check_serial{};
    // The package hold, written only by its owner thread under `mutex`.
    MetalHold hold{MetalHold::None};
    bool hold_lent_queue{};           // the hold lent the queue handle
    std::string hold_violation;       // first protocol violation of the hold
    std::vector<MetalCompletion> open_completions;

    // Core pipelines by MetalKernel id.
    std::mutex pipeline_mutex;
    std::unordered_map<std::uint32_t, id<MTLComputePipelineState>> pipelines;

    // Buffer pool: released buffers by size class, each reusable once
    // the command buffer with serial `retire_after` has completed.
    struct Pooled {
        id<MTLBuffer> buffer{nil};
        std::uint64_t retire_after{};
    };
    std::mutex pool_mutex;
    std::unordered_map<std::size_t, std::deque<Pooled>> pool;
    std::size_t pooled_bytes{};

    // Completion state, also touched by Metal's completion threads.
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::deque<MetalInFlight> in_flight;
    std::uint64_t submitted_serial{};
    std::atomic<std::uint64_t> completed_serial{0};
    std::string async_error;
    // Set with async_error, so the idle host-read path (which takes no
    // lock on the completion state) still reports a failed command buffer.
    std::atomic<bool> async_failed{false};

    // Package completion callbacks (qcore_device_on_complete) run on this
    // Core-owned serial queue, never inside Metal's completion handler: a
    // command buffer's slot in the queue's limit of uncompleted buffers is
    // freed only after its handler returns, and a callback may call back
    // into the stream (encode, defer a check, flush, wait), which can need
    // such a slot. Batches are counted when Metal's handler hands them over
    // (before completed_serial advances) and when they finish, so a
    // synchronization can wait for the callbacks of the work it waited for.
    dispatch_queue_t completion_queue{nullptr};
    std::atomic<std::uint64_t> completion_batches{0};
    std::atomic<std::uint64_t> completion_batches_done{0};
    // Callbacks registered and not yet run (the exit guard drains them).
    std::atomic<std::uint64_t> completions_pending{0};
};

// Records that the open command buffer references `buffer` (and the
// allocation a view lives in). Caller holds the stream mutex.
void metal_mark_use(const Buffer* buffer);

// Binds a Core buffer (or a view of one, such as a paged status word) at its
// base offset plus `offset`.
void metal_bind(id<MTLComputeCommandEncoder> encoder, const Buffer* buffer,
                std::size_t offset, NSUInteger index) {
    metal_mark_use(buffer);
    [encoder setBuffer:buffer->metal_buffer
                offset:buffer->base_offset + offset
               atIndex:index];
}

// Binds the whole native allocation; the kernel indexes from its start.
void metal_bind_whole(id<MTLComputeCommandEncoder> encoder, const Buffer* buffer,
                      NSUInteger index) {
    metal_mark_use(buffer);
    [encoder setBuffer:buffer->metal_buffer offset:0 atIndex:index];
}

template <typename Deleter>
void metal_bind(id<MTLComputeCommandEncoder> encoder,
                const std::unique_ptr<Buffer, Deleter>& buffer,
                std::size_t offset, NSUInteger index) {
    metal_bind(encoder, buffer.get(), offset, index);
}

// The stream this thread holds for a package encode, if any. A thread holds
// at most one (holds do not nest), and while it holds one, package calls
// that name another device are refused before anything locks that device's
// stream (package_hold_refuses_device), so a thread never waits for a second
// stream while it keeps one locked.
thread_local MetalStream* metal_held_stream = nullptr;

// The package holds Core's encoder or command buffer. Caller holds the
// stream mutex (or is the hold's owner thread).
bool metal_scope_open(const MetalStream& stream) {
    return stream.hold == MetalHold::Encoder ||
           stream.hold == MetalHold::CommandBuffer;
}

// Records a protocol violation of the calling thread's hold and returns its
// message: the first one is reported when the hold ends. Caller holds the
// stream mutex and the hold.
std::string metal_record_violation_locked(MetalStream& stream,
                                          std::string message) {
    counters::add(counters::Id::PackageScopeViolations); // qcount
    if (stream.hold_violation.empty()) stream.hold_violation = message;
    return message;
}

// Refuses Core work requested while the package holds Core's encoder or
// command buffer (encoding, uploads, copies, fills, blits, copy-on-write
// detaches). `work` names it. Caller holds the stream mutex.
bool metal_scope_refuses_locked(MetalStream& stream, const std::string& work,
                                std::string& error) {
    if (!metal_scope_open(stream)) return false;
    error = metal_record_violation_locked(stream,
        work + " was requested on gpu(" + std::to_string(stream.global_index) +
        ") while package code holds Core's " +
        (stream.hold == MetalHold::Encoder ? "compute encoder"
                                           : "command buffer") +
        ": Core does no work on a device stream inside a package encoder "
        "scope; take every tensor handle and status slot before "
        "qcore_device_compute_encoder or qcore_metal_command_buffer, and end "
        "the scope with qcore_metal_note_work before the extern call returns "
        "(include/quidra/native_extension.h)");
    return true;
}

// Refuses a commit or wait requested while a package holds the stream
// (a flush, a synchronization, a host read or write wait): every handle lent
// in the hold must stay used by the command buffer the package encodes
// into. Caller holds the stream mutex.
bool metal_hold_refuses_commit_locked(MetalStream& stream,
                                      const std::string& work,
                                      std::string& error) {
    if (stream.hold == MetalHold::None) return false;
    error = metal_record_violation_locked(stream,
        work + " was requested on gpu(" + std::to_string(stream.global_index) +
        ") while package code holds its command stream: Core cannot commit "
        "or wait for it until qcore_metal_note_work ends the encode "
        "(include/quidra/native_extension.h)");
    return true;
}

// The stream whose completion callbacks this thread is running, if any: a
// synchronization of that stream must not wait for its callbacks (itself).
// Per stream, so a callback of one device that waits for another device
// still waits for the other device's callbacks.
thread_local MetalStream* metal_running_completions = nullptr;

struct MetalCompletionBatch {
    MetalStream* stream{};
    std::vector<MetalCompletion> completions;
};

void metal_run_completion_batch(void* raw) {
    std::unique_ptr<MetalCompletionBatch> batch(
        static_cast<MetalCompletionBatch*>(raw));
    auto* stream = batch->stream;
    metal_running_completions = stream;
    // Package code outside any Quidra statement: a failure in it ends the
    // process at once (exit_failed_program).
    running_package_callback = true;
    @autoreleasepool {
        for (const auto& completion : batch->completions) {
            if (!completion.function) continue;
            completion.function(completion.context);
            check_package_hold_returned(
                "a qcore_device_on_complete callback returned");
        }
    }
    running_package_callback = false;
    metal_running_completions = nullptr;
    {
        std::lock_guard lock(stream->done_mutex);
        stream->completions_pending.fetch_sub(batch->completions.size(),
                                              std::memory_order_acq_rel);
        stream->completion_batches_done.fetch_add(1, std::memory_order_acq_rel);
    }
    stream->done_cv.notify_all();
}

// Hands callbacks to the completion queue. Caller holds done_mutex, and does
// this before completed_serial covers the work, so a waiter that sees the
// serial also sees the batch.
void metal_dispatch_completions_locked(MetalStream& stream,
                                       std::vector<MetalCompletion> completions) {
    if (completions.empty()) return;
    auto* batch = new MetalCompletionBatch{&stream, std::move(completions)};
    stream.completion_batches.fetch_add(1, std::memory_order_acq_rel);
    dispatch_async_f(stream.completion_queue, batch, metal_run_completion_batch);
}

// Waits until the completion callbacks handed over so far have run, so a
// synchronization returns only after the callbacks of the work it waited
// for. Skipped on the stream's own completion queue. Never called with a
// stream lock held (callbacks may take it).
void metal_drain_completions(MetalStream& stream) {
    if (metal_running_completions == &stream) return;
    const auto target = stream.completion_batches.load(std::memory_order_acquire);
    if (stream.completion_batches_done.load(std::memory_order_acquire) >= target)
        return;
    counters::add(counters::Id::PackageCompletionWaits); // qcount
    std::unique_lock lock(stream.done_mutex);
    stream.done_cv.wait(lock, [&] {
        return stream.completion_batches_done.load(std::memory_order_acquire) >=
               target;
    });
}

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
// Test-only: QUIDRA_TEST_METAL_FAIL_COMMAND=<serial> reports the command
// buffer with that serial as failed, so tests can check at which
// synchronization point a failed command buffer surfaces.
std::uint64_t metal_test_fail_serial() {
    static const std::uint64_t serial =
        metal_env_u64("QUIDRA_TEST_METAL_FAIL_COMMAND", 0);
    return serial;
}
#endif

// Metal's completion handler: retires serials in order and hands package
// callbacks to the completion queue. It never runs package code and takes no
// stream lock, so it always returns promptly.
void metal_command_completed(MetalStream* stream, std::uint64_t serial,
                             id<MTLCommandBuffer> command) {
    std::vector<id<MTLCommandBuffer>> retired;
    {
        std::lock_guard lock(stream->done_mutex);
        bool failed = [command status] == MTLCommandBufferStatusError;
        std::string test_failure;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
        if (!failed && serial == metal_test_fail_serial()) {
            failed = true;
            test_failure = "test-only Metal command failure";
        }
#endif
        if (failed && stream->async_error.empty()) {
            if (!test_failure.empty()) {
                stream->async_error = test_failure;
            } else {
                NSError* ns_error = [command error];
                stream->async_error = "Metal command failed";
                if (ns_error) {
                    stream->async_error += ": ";
                    stream->async_error += [[ns_error localizedDescription] UTF8String];
                }
            }
            stream->async_failed.store(true, std::memory_order_release);
        }
        for (auto& entry : stream->in_flight)
            if (entry.serial == serial) entry.done = true;
        // Serials retire in order, so completed_serial means "this one and
        // every earlier one finished".
        while (!stream->in_flight.empty() && stream->in_flight.front().done) {
            auto& front = stream->in_flight.front();
            metal_dispatch_completions_locked(*stream, std::move(front.completions));
            stream->completed_serial.store(front.serial, std::memory_order_release);
            retired.push_back(front.command);
            stream->in_flight.pop_front();
        }
    }
    stream->done_cv.notify_all();
    for (id<MTLCommandBuffer> value : retired) [value release];
}

// Commits `command` (retained by the caller, ownership passes to the stream)
// with the next serial. Caller holds stream.mutex.
void metal_submit_locked(MetalStream& stream, id<MTLCommandBuffer> command,
                         std::vector<MetalCompletion> completions) {
    const auto serial = stream.next_serial.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(stream.done_mutex);
        MetalInFlight entry;
        entry.serial = serial;
        entry.command = command;
        entry.completions = std::move(completions);
        stream.in_flight.push_back(std::move(entry));
        stream.submitted_serial = serial;
    }
    MetalStream* owner = &stream;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        metal_command_completed(owner, serial, completed);
    }];
    [command commit];
}

void metal_end_encoder_locked(MetalStream& stream) {
    if (!stream.encoder) return;
    [stream.encoder endEncoding];
    [stream.encoder release];
    stream.encoder = nil;
}

// Commits the open command buffer, if it holds any work. Never waits, and
// never runs while a package holds the stream.
void metal_commit_locked(MetalStream& stream) {
    if (!stream.open) return;
    metal_end_encoder_locked(stream);
    id<MTLCommandBuffer> command = stream.open;
    stream.open = nil;
    stream.ops = 0;
    stream.bytes = 0;
    auto completions = std::move(stream.open_completions);
    stream.open_completions.clear();
    counters::add(counters::Id::MetalCommandBuffers); // qcount
    metal_submit_locked(stream, command, std::move(completions));
}

// An empty command buffer committed after package-committed work, so a wait
// on it (or a completion attached to it) covers that work too.
bool metal_commit_marker_locked(MetalStream& stream, std::string& error,
                                std::vector<MetalCompletion> completions = {}) {
    id<MTLCommandBuffer> marker = [stream.queue commandBuffer];
    if (!marker) {
        error = "Metal synchronization command creation failed";
        return false;
    }
    [marker retain];
    counters::add(counters::Id::MetalMarkerBuffers); // qcount
    metal_submit_locked(stream, marker, std::move(completions));
    return true;
}

// Commits whatever covers all work submitted so far, including package
// command buffers Core did not see: the open command buffer, or a marker
// when packages may have committed their own (or a deferred check waits for
// a serial nothing has been submitted under yet). Returns the serial whose
// completion covers that work. Caller holds stream.mutex, outside a scope.
bool metal_commit_cover_locked(MetalStream& stream, std::uint64_t& target,
                               std::string& error) {
    if (stream.open) {
        metal_commit_locked(stream);
    } else if (stream.package_lent ||
               stream.check_serial >=
                   stream.next_serial.load(std::memory_order_relaxed)) {
        if (!metal_commit_marker_locked(stream, error)) return false;
    }
    target = stream.next_serial.load(std::memory_order_relaxed) - 1;
    return true;
}

// Discards the open command buffer of a hold that broke the protocol:
// nothing encoded in it runs (the package may have bound refused handles,
// or left an encoder of its own open). The program stops right after, so
// the command buffer and Core's encoder are dropped, never committed or
// ended under the package. Caller holds the stream mutex and the hold.
void metal_discard_open_locked(MetalStream& stream) {
    stream.encoder = nil;
    stream.open = nil;
    stream.ops = 0;
    stream.bytes = 0;
    stream.completions_pending.fetch_sub(stream.open_completions.size(),
                                         std::memory_order_acq_rel);
    stream.open_completions.clear();
}

bool metal_open_locked(MetalStream& stream, std::string& error) {
    if (stream.open) return true;
    id<MTLCommandBuffer> command = [stream.queue commandBuffer];
    if (!command) {
        error = "Metal command buffer creation failed";
        return false;
    }
    stream.open = [command retain];
    stream.opened_ns = metal_now_ns();
    return true;
}

// Core's compute encoder on the open command buffer. Refused for Core work
// while a package holds the encoder or the command buffer (the encoder's
// state cannot be restored for the package). `core_work` is false only when
// a package opens its encoder scope.
id<MTLComputeCommandEncoder> metal_compute_encoder_locked(
    MetalStream& stream, std::string& error, bool core_work = true) {
    if (core_work && metal_scope_refuses_locked(stream, "a Core compute kernel", error))
        return nil;
    if (!metal_open_locked(stream, error)) return nil;
    if (stream.encoder) return stream.encoder;
    id<MTLComputeCommandEncoder> encoder = [stream.open computeCommandEncoder];
    if (!encoder) {
        error = "Metal compute command creation failed";
        return nil;
    }
    counters::add(counters::Id::MetalComputeEncoders); // qcount
    stream.encoder = [encoder retain];
    return stream.encoder;
}

// Ends Core's compute encoder, so never while the package holds it (or the
// command buffer): the package would go on encoding on a released encoder.
id<MTLBlitCommandEncoder> metal_blit_encoder_locked(
    MetalStream& stream, std::string& error) {
    if (metal_scope_refuses_locked(stream, "a Core blit", error)) return nil;
    if (!metal_open_locked(stream, error)) return nil;
    metal_end_encoder_locked(stream);
    id<MTLBlitCommandEncoder> encoder = [stream.open blitCommandEncoder];
    if (!encoder) {
        error = "Metal copy command creation failed";
        return nil;
    }
    counters::add(counters::Id::MetalBlitEncoders); // qcount
    return encoder;
}

// Called after one unit of work was appended to the open command buffer.
void metal_note_work_locked(MetalStream& stream, std::uint32_t ops,
                            std::uint64_t bytes) {
    stream.ops += ops;
    stream.bytes += bytes;
    stream.work_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (stream.hold != MetalHold::None) return; // commits wait for the hold to end
    const auto& config = metal_stream_config();
    bool commit = stream.ops >= config.max_ops || stream.bytes >= config.max_bytes;
    if (!commit && config.stream &&
        stream.completed_serial.load(std::memory_order_acquire) ==
            stream.next_serial.load(std::memory_order_relaxed) - 1) {
        // The GPU has nothing left to run: hand it this batch so host
        // encoding and GPU execution overlap. The op/time floor keeps a
        // host-bound loop from degenerating into one buffer per op.
        commit = stream.ops >= config.idle_ops ||
                 metal_now_ns() - stream.opened_ns >= config.idle_ns;
    }
    if (commit) metal_commit_locked(stream);
}

std::vector<MetalStream*> create_metal_streams() {
    std::vector<MetalStream*> streams;
    @autoreleasepool {
        NSArray<id<MTLDevice>>* metal_devices = MTLCopyAllDevices();
        for (NSUInteger i = 0; i < [metal_devices count]; ++i) {
            auto* stream = new MetalStream;
            stream->backend_index = static_cast<int>(i);
            stream->device = [[metal_devices objectAtIndex:i] retain];
            stream->queue = [stream->device newCommandQueue];
            stream->completion_queue = dispatch_queue_create(
                "org.quidra.metal.completions", DISPATCH_QUEUE_SERIAL);
            streams.push_back(stream);
        }
        [metal_devices release];
    }
    for (const auto& info : devices())
        if (info.backend == Backend::Metal && info.backend_index >= 0 &&
            static_cast<std::size_t>(info.backend_index) < streams.size())
            streams[static_cast<std::size_t>(info.backend_index)]->global_index = info.index;
    return streams;
}

// Waits silently for all queued work at process teardown, as the per-op
// command buffers did before.
struct MetalStreamTeardown {
    std::vector<MetalStream*>* streams{};
    ~MetalStreamTeardown();
};

// Set once the streams exist, so exit-time queries never create them.
std::atomic<std::vector<MetalStream*>*> metal_streams_created{nullptr};

std::vector<MetalStream*>& metal_streams() {
    static auto* streams = new std::vector<MetalStream*>(create_metal_streams());
    static MetalStreamTeardown teardown{streams};
    (void)teardown;
    metal_streams_created.store(streams, std::memory_order_release);
    return *streams;
}

MetalStream* metal_stream(int backend_index) {
    auto& streams = metal_streams();
    if (backend_index < 0 ||
        static_cast<std::size_t>(backend_index) >= streams.size())
        return nullptr;
    auto* stream = streams[static_cast<std::size_t>(backend_index)];
    return stream->queue ? stream : nullptr;
}

MetalStream* metal_stream_for(const Buffer* buffer) {
    return buffer ? metal_stream(buffer->backend_index) : nullptr;
}

bool metal_take_async_error(MetalStream& stream, std::string& error) {
    std::lock_guard lock(stream.done_mutex);
    if (stream.async_error.empty()) return true;
    error = std::move(stream.async_error);
    stream.async_error.clear();
    stream.async_failed.store(false, std::memory_order_release);
    return false;
}

// Reports a failed command buffer without clearing it, so the next
// synchronization point of the program still reports it.
bool metal_peek_async_error(MetalStream& stream, std::string& error) {
    std::lock_guard lock(stream.done_mutex);
    if (stream.async_error.empty()) return true;
    error = stream.async_error;
    return false;
}

void metal_wait_serial(MetalStream& stream, std::uint64_t target) {
    if (target == 0 ||
        stream.completed_serial.load(std::memory_order_acquire) >= target)
        return;
    std::unique_lock lock(stream.done_mutex);
    stream.done_cv.wait(lock, [&] {
        return stream.completed_serial.load(std::memory_order_acquire) >= target;
    });
}

// Commits the open command buffer (or a marker covering package work) and
// waits for everything submitted so far, and for the package completion
// callbacks of that work. Concurrent callers each wait for the serial they
// observed, so none returns before its work finished. `covered` receives
// that serial: deferred checks queued up to it can be read now (a check a
// callback deferred encodes new work, which a later synchronization covers).
// A command buffer that failed is reported, and cleared with
// `consume_error`. Package waits leave it for the program's next
// synchronization point, as they leave deferred checks: a package (or a
// completion callback) cannot raise it into the program.
bool metal_stream_synchronize(MetalStream& stream, counters::WaitReason reason,
                              std::string& error,
                              std::uint64_t* covered = nullptr,
                              bool consume_error = true) {
    std::uint64_t target = 0;
    {
        std::lock_guard lock(stream.mutex);
        if (metal_hold_refuses_commit_locked(stream, "a Metal synchronization", error))
            return false;
        if (!metal_commit_cover_locked(stream, target, error)) return false;
    }
    if (stream.completed_serial.load(std::memory_order_acquire) < target) {
        counters::note_wait(reason); // qcount
        metal_wait_serial(stream, target);
    }
    metal_drain_completions(stream);
    if (covered) *covered = target;
    return consume_error ? metal_take_async_error(stream, error)
                         : metal_peek_async_error(stream, error);
}

// True while package completion callbacks are registered and have not run.
// Never creates the streams (the exit guard asks for every Metal device).
bool metal_completions_pending(int backend_index) {
    auto* streams = metal_streams_created.load(std::memory_order_acquire);
    if (!streams || backend_index < 0 ||
        static_cast<std::size_t>(backend_index) >= streams->size())
        return false;
    return (*streams)[static_cast<std::size_t>(backend_index)]
               ->completions_pending.load(std::memory_order_acquire) != 0;
}

// The serial whose completion makes a status word readable, for a check
// deferred now. A word Core bound in a dispatch was written by the command
// buffer it was bound in. A word package code wrote is covered by the next
// command buffer Core commits: the package encoded its kernel into Core's
// open buffer, or committed its own before deferring the check.
std::uint64_t metal_check_serial(MetalStream& stream, const Buffer& status) {
    std::lock_guard lock(stream.mutex);
    const auto next = stream.next_serial.load(std::memory_order_relaxed);
    const auto serial = status.last_use != 0 ? status.last_use : next;
    if (serial >= next) stream.check_serial = std::max(stream.check_serial, serial);
    return serial;
}

// A native handle is being lent to package code. Outside a package hold
// that code may commit its own command buffer on Core's queue; it must run
// after everything Core has encoded so far, so the open buffer is committed
// first, and every later synchronization must cover what the package commits.
void metal_lend_to_package(MetalStream& stream, const Buffer* buffer) {
    std::lock_guard lock(stream.mutex);
    stream.work_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (stream.hold != MetalHold::None) {
        // In a hold (this thread's: it holds the lock) a buffer handle is
        // bound only in the hold's scope, which encodes into the command
        // buffer Core opens next or has open (nothing is committed during a
        // hold), so its use is recorded like a Core binding, and the host may
        // touch the buffer again once that command buffer has completed. A
        // queue handle can serve a command buffer the package commits after
        // the hold: the hold's end commits Core's open work first.
        if (buffer) {
            metal_mark_use(buffer);
        } else {
            stream.package_lent = true;
            stream.hold_lent_queue = true;
        }
        return;
    }
    // Package command buffers are not tracked per buffer: the host may touch
    // a lent buffer only after a full synchronization from now on. Marked
    // under the stream lock, which every idle check holds.
    if (buffer) {
        const_cast<Buffer*>(buffer)->exposed = true;
        if (buffer->owner) buffer->owner->exposed = true;
    }
    if (stream.open) {
        counters::add(counters::Id::PackageBorrowFlushes); // qcount
        metal_commit_locked(stream);
    }
    stream.package_lent = true;
}

MetalStreamTeardown::~MetalStreamTeardown() {
    if (!streams) return;
    // A hold the exiting thread still has would refuse the synchronization
    // below; it is a protocol violation of its own.
    check_package_hold_returned("the program exited");
    for (auto* stream : *streams) {
        std::string ignored;
        (void)metal_stream_synchronize(*stream, counters::WaitReason::Sync, ignored);
    }
}

// Allocates shared (unified-memory) storage on the cached device.
id<MTLBuffer> metal_new_buffer(int backend_index, std::size_t bytes,
                               std::string& error) {
    auto* stream = metal_stream(backend_index);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return nil;
    }
    id<MTLBuffer> buffer = [stream->device newBufferWithLength:bytes
                                                      options:MTLResourceStorageModeShared];
    if (!buffer) {
        error = "Metal GPU memory allocation failed";
        return nil;
    }
    counters::add(counters::Id::MetalNewBuffers); // qcount
    return buffer;
}

struct MetalPoolConfig {
    bool enabled{true};
    std::size_t capacity{256ull << 20};
    bool poison{};
    unsigned char poison_byte{};
};

const MetalPoolConfig& metal_pool_config() {
    static const MetalPoolConfig config = [] {
        MetalPoolConfig value;
        if (const auto pool = platform::environment_value("QUIDRA_METAL_POOL"))
            value.enabled = *pool != "0";
        value.capacity = static_cast<std::size_t>(
            metal_env_u64("QUIDRA_METAL_POOL_BYTES", value.capacity));
        if (const auto poison =
                platform::environment_value("QUIDRA_TEST_POOL_POISON")) {
            const char* text = poison->c_str();
            char* end = nullptr;
            const auto byte = std::strtoul(text, &end, 0);
            if (end != text) {
                value.poison = true;
                value.poison_byte = static_cast<unsigned char>(byte);
            }
        }
        return value;
    }();
    return config;
}

// Size classes: 256 B, then four classes per power of two up to 64 MiB
// (1.25, 1.5, 1.75 and 2 times the power below), then 64 KiB multiples. A
// buffer is at most 25% larger than its request (power-of-two classes
// wasted up to 100%, so peak device memory could nearly double), and equal
// requests still share one class.
std::size_t metal_pool_class(std::size_t bytes) {
    constexpr std::size_t minimum = 256;
    constexpr std::size_t quarter_limit = 64ull << 20;
    if (bytes <= minimum) return minimum;
    if (bytes <= quarter_limit) {
        const auto step = std::max<std::size_t>(
            minimum / 4, std::bit_floor(bytes - 1) / 4);
        return (bytes + step - 1) / step * step;
    }
    constexpr std::size_t granule = 64ull << 10;
    return (bytes + granule - 1) / granule * granule;
}

// A released buffer may still be referenced by the open command buffer, by
// a committed one, or by a command buffer that package code committed on
// Core's queue before the release. All of them precede the next command
// buffer Core commits, so the buffer records retire_after = the serial of
// that command buffer: once it has completed, nothing on the GPU uses the
// buffer any more.
void metal_pool_give(MetalStream& stream, id<MTLBuffer> buffer,
                     std::size_t class_bytes) {
    const auto& config = metal_pool_config();
    if (!config.enabled || class_bytes == 0) {
        [buffer release];
        return;
    }
    const auto retire_after = stream.next_serial.load(std::memory_order_acquire);
    std::lock_guard lock(stream.pool_mutex);
    if (stream.pooled_bytes + class_bytes > config.capacity) {
        [buffer release];
        return;
    }
    stream.pool[class_bytes].push_back(MetalStream::Pooled{buffer, retire_after});
    stream.pooled_bytes += class_bytes;
}

// Takes the oldest pooled buffer of the class. Device work on Core's queue
// is ordered after every earlier use of the buffer (one in-order queue, and
// Metal tracks write-after-read hazards on these tracked resources), so a
// buffer whose last command buffer has not completed yet can still back a
// new tensor: `busy_until` then carries that serial, and the host may touch
// the bytes only once it has completed (metal_buffer_idle_locked). With
// `require_idle` only a retired buffer is taken (host staging).
id<MTLBuffer> metal_pool_take(MetalStream& stream, std::size_t class_bytes,
                              bool require_idle, std::uint64_t& busy_until) {
    busy_until = 0;
    if (!metal_pool_config().enabled) return nil;
    const auto completed = stream.completed_serial.load(std::memory_order_acquire);
    std::lock_guard lock(stream.pool_mutex);
    auto found = stream.pool.find(class_bytes);
    if (found == stream.pool.end()) return nil;
    auto& entries = found->second;
    if (entries.empty()) return nil;
    const auto retire_after = entries.front().retire_after;
    if (retire_after > completed) {
        if (require_idle) return nil;
        busy_until = retire_after;
    }
    id<MTLBuffer> buffer = entries.front().buffer;
    entries.pop_front();
    stream.pooled_bytes -= class_bytes;
    return buffer;
}

// Test mode (QUIDRA_TEST_POOL_POISON): overwrites a pooled buffer that queued
// work may still use with the poison byte, in queue order after that work,
// and moves `busy_until` to the poisoning command buffer. Defined with the
// Core kernels.
void metal_poison_in_queue(MetalStream& stream, id<MTLBuffer> buffer,
                           std::size_t bytes, unsigned char value,
                           std::uint64_t& busy_until);

// Allocation entry for Metal storage: a pooled buffer or a new one.
// `class_bytes` receives the physical size to return to the pool and
// `busy_until` the serial the host must wait for before touching the bytes
// (0: idle now).
id<MTLBuffer> metal_acquire_buffer(int backend_index, std::size_t bytes,
                                   bool require_idle, std::size_t& class_bytes,
                                   std::uint64_t& busy_until, std::string& error) {
    auto* stream = metal_stream(backend_index);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return nil;
    }
    const auto& config = metal_pool_config();
    class_bytes = config.enabled ? metal_pool_class(bytes) : bytes;
    id<MTLBuffer> buffer =
        metal_pool_take(*stream, class_bytes, require_idle, busy_until);
    if (buffer) {
        counters::add(counters::Id::MetalPoolHits); // qcount
    } else {
        buffer = metal_new_buffer(backend_index, class_bytes, error);
        if (!buffer) return nil;
    }
    // Poison test mode: idle bytes are poisoned on the host, bytes queued
    // work may still use by a kernel ordered after that work (the queue-order
    // reuse path), so both paths prove that outputs are completely written.
    if (config.poison) {
        if (busy_until == 0)
            std::memset([buffer contents], config.poison_byte, class_bytes);
        else
            metal_poison_in_queue(*stream, buffer, class_bytes,
                                  config.poison_byte, busy_until);
    }
    return buffer;
}

void metal_release_buffer(int backend_index, id<MTLBuffer> buffer,
                          std::size_t class_bytes) {
    auto* stream = metal_stream(backend_index);
    if (!stream) {
        [buffer release];
        return;
    }
    metal_pool_give(*stream, buffer, class_bytes);
}

void metal_mark_use(const Buffer* buffer) {
    auto* stream = metal_stream(buffer->backend_index);
    if (!stream) return;
    const auto serial = stream->next_serial.load(std::memory_order_relaxed);
    const_cast<Buffer*>(buffer)->last_use = serial;
    if (buffer->owner) buffer->owner->last_use = serial;
}

// True when no queued GPU work can read or write `buffer`: Core never bound
// it in a command buffer that is still open or running, and no package code
// received its native handle (package command buffers are not tracked per
// buffer). Caller holds the stream mutex.
bool metal_buffer_idle_locked(const MetalStream& stream, const Buffer& buffer) {
    if (buffer.exposed) return false;
    const auto use = buffer.last_use;
    if (use == 0) return true;
    return use < stream.next_serial.load(std::memory_order_relaxed) &&
           use <= stream.completed_serial.load(std::memory_order_acquire);
}
