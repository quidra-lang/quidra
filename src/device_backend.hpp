#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quidra::device {

enum class ExecutionMode { Fast, Deterministic };

// Members are named after the canonical backend ids in project.toml rather than
// after the vendors, so the identifier a human reads here is the same one that
// appears in machine-readable metadata. `backend_display_name` holds the
// separate, human-facing spelling.
enum class Backend {
    Cuda,
    Hip,
    Metal,
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    Test,
#endif
};

struct Info {
    int index{};
    Backend backend{Backend::Cuda};
    int backend_index{};
    std::string name;
    std::string driver;
    std::string runtime;
};

struct Buffer;
struct Module;

struct LaunchDimensions {
    unsigned x{1};
    unsigned y{1};
    unsigned z{1};
};

const std::vector<Info>& devices();
const Info* find(int index);

// Human-facing name, matching the [[backends]] display value in project.toml.
std::string backend_display_name(Backend backend);

void set_execution_mode(ExecutionMode mode);
ExecutionMode execution_mode();

bool synchronize(int index, std::string& error);
bool synchronize_all(std::string& error);

Buffer* allocate(int index, std::size_t bytes, std::string& error);
// allocate(), optionally refusing Metal pool buffers that queued GPU work may
// still use (`require_idle`), so the host can write the bytes at once. Package
// scratch (qcore_device_buffer_allocate) is allocated idle: before the pool,
// it was always a fresh buffer that no queued work used.
Buffer* allocate_buffer(int index, std::size_t bytes, bool require_idle,
                        std::string& error);
void release(Buffer* buffer);
bool copy_from_host(Buffer* buffer, std::size_t offset, const void* source,
                    std::size_t bytes, std::string& error);
bool copy_to_host(const Buffer* buffer, std::size_t offset, void* destination,
                  std::size_t bytes, std::string& error);
bool copy_device_to_device(Buffer* destination, std::size_t destination_offset,
                           const Buffer* source, std::size_t source_offset,
                           std::size_t bytes, std::string& error);
bool zero(Buffer* buffer, std::size_t offset, std::size_t bytes,
          std::string& error);
// True when outputs that a Core kernel writes completely may skip their zero
// fill on this device (Metal and the fake backend; CUDA/HIP keep the fill).
// QUIDRA_GPU_ZERO_FILL=always keeps every fill.
bool written_outputs_skip_fill(int index);
int buffer_device(const Buffer* buffer);
Backend buffer_backend(const Buffer* buffer);
std::uint64_t buffer_native_handle(const Buffer* buffer);
// Byte offset of the buffer inside its native allocation (add it to every
// binding of buffer_native_handle).
std::size_t buffer_base_offset(const Buffer* buffer);
std::uint64_t queue_native_handle(int index);
// Borrowed backend-native device object (id<MTLDevice> on Metal), 0 on
// other backends. Lends no queue: work cannot be ordered through it.
std::uint64_t native_device_handle(int index);
bool activate(int index, std::string& error);

// Package warm-up functions (qcore_register_warmup): each runs once per GPU
// device on a Core background thread, at the device's first use by the
// program, or at registration for devices already in use.
void register_warmup(void (*function)(long long device));

// Core's per-device command stream as seen by package code (the protocol is
// specified in native_extension.h). On Metal a package encode appends to
// Core's open command buffer, and the calling thread holds the device stream
// from hold_package_stream, or from begin_package_encode without one, until
// end_package_encode.
//
// Holds the stream for a package encode: Core may still work on it (handle
// acquisition) but commits nothing. False when the backend has no such
// stream or it is disabled (QUIDRA_METAL_STREAM=0), and when the thread
// already holds one (a protocol violation, recorded on that hold).
bool hold_package_stream(int index);
// Opens the package encoder scope: Core's open id<MTLComputeCommandEncoder>,
// or with `command_buffer` its open id<MTLCommandBuffer> (Core's encoder is
// ended). From then until end_package_encode Core does no work on the
// stream. Takes the hold first when the thread has none. Returns 0 when the
// backend has no such stream, on failure, and on a protocol violation.
std::uint64_t begin_package_encode(int index, bool command_buffer,
                                   std::string& error);
// Ends the hold. On a hold that broke the protocol it returns false with
// `violated` set and the first violation in `error`; the hold's open
// command buffer was discarded, and the caller stops the program. On a
// package callback thread (running_package_callback) such a hold ends the
// process here instead (fail_package_scope_unlocated), with the stream
// still held. On a task.all task thread (running_task_all_task) the stream
// stays held and nothing is discarded: the caller's report ends the process
// at once (exit_failed_program).
bool end_package_encode(int index, std::uint32_t dispatches,
                        std::uint64_t bytes, bool& violated,
                        std::string& error);
// True while the calling thread holds gpu(index)'s package encoder scope
// open, where any Core work on that stream is a protocol violation.
bool package_scope_open(int index);
// Records a protocol violation for `work` (Core work the caller refused
// inside the calling thread's open package encoder scope on gpu(index)).
void note_package_scope_violation(int index, const std::string& work);
// True while the calling thread holds a device stream for a package encode
// (from the hold to qcore_metal_note_work, scope included).
bool package_hold_active();
// Records a protocol violation for `work` on the calling thread's hold, if
// it has one, and returns whether it did.
bool note_package_hold_violation(const std::string& work);
// A hold covers one device: while the calling thread holds a device stream,
// `call` naming another device (gpu(index)) is refused and recorded as a
// protocol violation, so a thread never waits for a second stream while it
// keeps one locked. True when refused.
bool package_hold_refuses_device(int index, const std::string& call);

// Package encode holds open in the process. The runtime reads it where
// control returns from package code to Quidra code (statement boundaries,
// the end of a task.all task, the return of a custom autograd backward
// callback, the start of task.all, its failure paths) before it asks
// whether the calling thread left a hold open, so that check costs one load
// otherwise.
inline std::atomic<unsigned> open_package_holds{0};
// Deferred device checks pending in the process, on every device: written
// with the pending list under its lock, read without it where an exported
// function returns to C, so that the boundary costs one load while nothing
// is pending.
inline std::atomic<std::size_t> pending_deferred_validations{0};
// Synchronizes every device with pending deferred checks (and, on Metal,
// pending completion callbacks), consumes the checks those synchronizations
// covered, and repeats until nothing is pending, as the exit guard does.
// Returns false with the first failure in `error`.
bool drain_deferred_validations(std::string& error);
enum class LeftHold { None, Open, Violated };
// When the calling thread still holds a device stream for a package encode,
// ends that hold: its open command buffer is discarded (nothing encoded in
// it runs), the stream is released, and `message` receives the first
// protocol violation the hold recorded (Violated) or a description of the
// encode left open, naming `returned` as the code that returned without
// qcore_metal_note_work (Open). The caller stops the program (GPU_SCOPE).
// On a package callback thread (running_package_callback) and on a task.all
// task thread (running_task_all_task) the stream stays held instead, and
// the hold is not ended or reported again: the process ends there without
// exit handlers (exit_failed_program), so no other thread waits for or
// reads the work of the command buffer that is never committed.
LeftHold end_left_package_hold(const char* returned, std::string& message);
// For package code that Core calls on a thread that runs no Quidra code
// (completion callbacks, warm-ups) and for exit: stops the program with
// GPU_SCOPE naming the root file (no statement runs) when the calling thread still holds
// a stream for a package encode. Returns otherwise. An exit handler that
// waits for another thread calls it first: that thread may be waiting for
// the stream the exiting thread holds.
void check_package_hold_returned(const char* returned);
// Set while this thread runs package code that Core calls outside any Quidra
// statement: a warm-up function (qcore_register_warmup) or a
// qcore_device_on_complete callback. A failure there has no statement (the
// report names the root file), and its thread must not run exit handlers while the program runs on
// (they wait for Core threads: a warm-up thread would join itself), so the
// process ends at once (exit_failed_program).
inline thread_local bool running_package_callback = false;
// Set on the threads that run task.all tasks (runtime.cpp). The other tasks
// run Quidra code on while a failing thread runs exit handlers, so a hold
// this thread breaks or leaves open stays held, and the process ends at once
// after the report (exit_failed_program): no other task waits for the
// stream, or reads a value whose work the hold's command buffer discarded.
inline thread_local bool running_task_all_task = false;
// Ends the process with status abi::failure_exit_status after a coded
// runtime failure was printed: std::exit, or std::_Exit after flushing stdout
// and stderr on a package callback thread (running_package_callback), on a
// task.all task thread (running_task_all_task), whose sibling tasks run on
// while exit handlers would run, and on a thread that keeps the hold whose
// failure it reports (end_left_package_hold).
[[noreturn]] void exit_failed_program();
// Reports GPU_SCOPE with <message> through the runtime's reporter, naming
// the root file (no statement runs), and ends the process at once
// (std::_Exit with abi::failure_exit_status). A stream the thread holds
// stays held.
[[noreturn]] void fail_package_scope_unlocated(const std::string& message);
// Commits queued work without waiting.
bool flush(int index, std::string& error);
// Commits and waits for all work queued on the device, including command
// buffers package code committed on Core's queue. Unlike synchronize(), it
// does not consume deferred validation results (Metal and the fake backend).
bool wait_idle(int index, std::string& error);
// Runs `function(context)` once all work queued so far has completed, or
// immediately when nothing is pending. On Metal it runs on the device's
// completion queue, and synchronizations that wait for the work also wait
// for the callback.
bool on_complete(int index, void (*function)(void*), void* context,
                 std::string& error);

// Device status words for checked package kernels (see native_extension.h).
bool status_slot(int index, std::uint64_t& handle, std::uint64_t& offset_bytes,
                 std::uint64_t& slot, std::string& error);
bool defer_status(int index, std::uint64_t slot, const char* message,
                  std::string& error);
bool status_wait(int index, std::uint64_t slot, std::uint32_t& value,
                 std::string& error);
void status_release(int index, std::uint64_t slot);

// Unified memory (src/device_unified.inc). A host-visible device keeps every
// buffer in memory the CPU can address coherently at command-buffer
// boundaries: Metal on unified-memory GPUs, and the test-only fake backend
// under QUIDRA_TEST_FAKE_GPU_UNIFIED=1. CUDA and HIP are never host-visible.
bool host_visible(int index);
// Host address of byte 0 of a host-visible buffer; nullptr otherwise.
unsigned char* host_address(Buffer* buffer);
// Validates and synchronizes exactly as copy_to_host does before it reads
// [0, bytes), without copying anything, for a host view that will outlive the
// call. Fails exactly when copy_to_host would (same message). On success
// `settled` reports whether the synchronization also proved that no device
// work Core submitted so far can still write the buffer, even while another
// thread waits for it. When it is false the caller must copy instead, because
// only a copy keeps the previous behaviour then. Package code that received
// the buffer's native handle may commit work with it after this returns, so
// the runtime never views such buffers at all.
bool prepare_host_view(const Buffer* buffer, std::size_t bytes,
                       std::string& error, bool& settled);
// An upload to gpu(index) was served as a view, so it committed no staging
// copy. A hook for backends whose next synchronization would otherwise wait
// less than after the copy. Metal's command stream needs nothing (an upload
// there commits no command buffer either, and once a handle was lent every
// synchronization covers package command buffers), and the fake GPU finishes
// every operation before returning, so it does nothing today. Leaves
// submission_serial unchanged.
void note_uncopied_upload(int index);
// A counter that changes whenever device work that may reference a buffer is
// encoded on gpu(index) (Metal: the stream's work epoch, which also changes
// when a native handle is lent), before it is committed. Package borrows are
// also recorded by the runtime itself.
// unknown_submission_serial means the backend cannot tell (always "changed").
inline constexpr std::uint64_t unknown_submission_serial = ~std::uint64_t{0};
std::uint64_t submission_serial(int index);
// Waits until device work submitted so far no longer reads the buffer, so the
// host may overwrite it in place. Asynchronous errors are left for the next
// synchronization point, exactly where they would surface without the wait.
bool wait_for_host_write(const Buffer* buffer, std::string& error);
// Alignment that host memory must have (for both its address and its
// capacity) to be wrapped by wrap_host_memory; 0 when wrapping is unavailable.
std::size_t host_wrap_granularity(int index);
// Creates a buffer on gpu(index) over existing host memory. On success the
// buffer owns the allocation and frees it with `deallocate` once the buffer
// and every submitted use of it are gone.
Buffer* wrap_host_memory(int index, unsigned char* data, std::size_t bytes,
                         std::size_t capacity,
                         void (*deallocate)(void* pointer, std::size_t capacity),
                         std::string& error);

Module* load_ptx(int index, const std::string& ptx, std::string& error);
Module* load_hip_source(int index, const std::string& source, std::string& error);
void release(Module* module);
bool launch(Module* module, const char* kernel,
            LaunchDimensions grid, LaunchDimensions block,
            void** arguments, std::string& error);

// Backend-neutral tensor compute primitives. All operations keep results on the
// same gpu(n); unsupported backend/dtype combinations fail explicitly.
bool compute_fill_ones(Buffer* output, int dtype, std::size_t count,
                       std::string& error);
bool compute_gather(Buffer* output, const Buffer* source, int dtype,
                    const std::uint64_t* source_indices, std::size_t count,
                    std::string& error);
bool compute_gather_backward(Buffer* input_gradient, const Buffer* output_gradient,
                             int dtype, const std::uint64_t* source_indices,
                             std::size_t source_count, std::size_t output_count,
                             std::string& error);
bool compute_compare_all(const Buffer* left, std::size_t left_offset,
                         const Buffer* right, std::size_t right_offset,
                         int dtype, int operation, std::size_t count,
                         bool& result, std::string& error);
bool compute_binary(Buffer* output, const Buffer* left, std::size_t left_offset,
                    const Buffer* right, std::size_t right_offset,
                    const void* scalar, int scalar_side, int dtype,
                    int operation, std::size_t count, std::string& error);
bool compute_unary(Buffer* output, const Buffer* input, std::size_t input_offset,
                   int dtype, int operation, std::size_t count,
                   std::string& error);
bool compute_cast(Buffer* output, const Buffer* input, std::size_t input_offset,
                  int source_dtype, int target_dtype, std::size_t count,
                  std::string& error);
bool compute_binary_backward(
    Buffer* left_gradient, Buffer* right_gradient,
    const Buffer* gradient, const Buffer* left, const Buffer* right,
    int dtype, int operation, std::size_t count, std::string& error);
bool compute_scalar_backward(
    Buffer* output, const Buffer* gradient, const Buffer* input,
    int dtype, int operation, bool scalar_left, double scalar,
    std::size_t count, std::string& error);

// Strided tensor access. Operand element i of a row-major `shape` lives at
// base + sum_axis coordinate_axis(i) * strides[axis] (in elements), so a
// stride of 0 broadcasts an axis and transposed or stepped views read in
// place. Implemented where supports_strided_compute() holds (Metal and the
// test backend); callers keep host-built gather indices elsewhere.
bool supports_strided_compute(const Buffer* buffer);
// True when compute_binary_strided() implements (dtype, operation) on the
// buffer's backend with the same arithmetic as compute_binary().
bool supports_strided_binary(const Buffer* buffer, int dtype, int operation);
bool compute_strided_copy(Buffer* output, const Buffer* source, int dtype,
                          std::size_t source_base,
                          const std::vector<long long>& shape,
                          const std::vector<long long>& strides,
                          std::string& error);
bool compute_binary_strided(Buffer* output,
                            const Buffer* left, std::size_t left_base,
                            const std::vector<long long>& left_strides,
                            const Buffer* right, std::size_t right_base,
                            const std::vector<long long>& right_strides,
                            const void* scalar, int scalar_side, int dtype,
                            int operation, const std::vector<long long>& shape,
                            std::string& error);
// Gradient of a broadcast read: input_gradient[s] is the sum of
// output_gradient over the output elements broadcast from source element s,
// accumulated serially in ascending output index order from +0.0 (the order
// of compute_gather_backward over the equivalent gather indices). Float
// dtypes, where supports_strided_compute() holds.
bool compute_broadcast_backward(Buffer* input_gradient,
                                const Buffer* output_gradient, int dtype,
                                const std::vector<long long>& input_shape,
                                const std::vector<long long>& output_shape,
                                std::string& error);

} // namespace quidra::device
