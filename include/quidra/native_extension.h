#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIDRA_NATIVE_ABI_VERSION 1u

enum qcore_backend {
    QCORE_BACKEND_CPU = 0,
    QCORE_BACKEND_CUDA = 1,
    QCORE_BACKEND_HIP = 2,
    QCORE_BACKEND_METAL = 3,
    QCORE_BACKEND_TEST = 4
};

enum qcore_dtype {
    QCORE_DTYPE_INT64 = 1,
    QCORE_DTYPE_INT8 = 2,
    QCORE_DTYPE_INT16 = 3,
    QCORE_DTYPE_INT32 = 4,
    QCORE_DTYPE_UINT8 = 5,
    QCORE_DTYPE_UINT16 = 6,
    QCORE_DTYPE_UINT32 = 7,
    QCORE_DTYPE_UINT64 = 8,
    QCORE_DTYPE_FLOAT64 = 9,
    QCORE_DTYPE_FLOAT32 = 10,
    QCORE_DTYPE_BOOL = 11
};

unsigned int qcore_native_abi_version(void);

// Generic exact-real providers let packages own named mathematical constants
// without teaching Core their names or values. The callback returns a stable,
// NUL-terminated certified decimal prefix for an opcode, or NULL when the
// opcode is not owned by that provider. Core treats the final supplied decimal
// digit as a lower bound and the next decimal unit as the upper bound when it
// needs an exact ordering proof.
typedef const char* (*qcore_exact_real_atom_decimal_fn)(uint32_t opcode);
int qcore_exact_real_provider_register(
    const char* provider,
    qcore_exact_real_atom_decimal_fn atom_decimal);

// Optional provider-owned unary exact semantics. Evaluation is used only when
// an exact symbolic value must be observed as a finite floating approximation;
// it never makes an unproved exact comparison true.
typedef int (*qcore_exact_real_unary_float64_fn)(
    uint32_t opcode, double input, double* output);
typedef uint32_t (*qcore_exact_real_unary_flags_fn)(uint32_t opcode);

enum qcore_exact_real_unary_flag {
    QCORE_EXACT_UNARY_TOTAL = 1u << 0,
    QCORE_EXACT_UNARY_DOMAIN_POSITIVE = 1u << 1,
    QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE = 1u << 2,
    QCORE_EXACT_UNARY_RESULT_POSITIVE = 1u << 3,
    QCORE_EXACT_UNARY_RESULT_NONNEGATIVE = 1u << 4
};

int qcore_exact_real_provider_register_unary(
    const char* provider,
    qcore_exact_real_unary_float64_fn evaluate,
    qcore_exact_real_unary_flags_fn flags);

// Construct a provider-owned symbolic unary exact-real node. Core owns the
// opaque exact expression and proof machinery; provider/opcode semantics stay
// entirely in the registering package.
void* qcore_exact_real_unary(
    const char* provider,
    uint32_t opcode,
    const void* input);

int qcore_tensor_dtype(const void* tensor);
long long qcore_tensor_device(const void* tensor);
unsigned long long qcore_tensor_rank(const void* tensor);
long long qcore_tensor_extent(const void* tensor, unsigned long long axis);
unsigned long long qcore_tensor_element_count(const void* tensor);
int qcore_tensor_is_contiguous(const void* tensor);

// Device-native access is backend-neutral. Handles are borrowed for the
// duration of the Quidra extern call and remain owned by Core. Mutable access
// applies Core's generic copy-on-write boundary before exposing storage.
// Like the CPU accessors below, both handle accessors return 0 for a view
// that contains an uninitialized element. Core reports nothing at that point:
// the package must treat 0 as a refusal and either take a portable path that
// reads through Core (which reports the uninitialized read) or fail the call
// itself.
// A borrow may also first give the tensor memory of its own: on unified memory
// the result of an explicit .gpu()/.cpu() can share its allocation with its
// source until its first borrow. That happens before any pointer or handle to
// the tensor is handed out, so do not compare them with ones from earlier
// calls. Core never lets a CPU tensor share memory whose device handle package
// code has received, so work encoded with a handle may still be committed in
// a later call, as before. A Metal buffer may be longer than the tensor's
// bytes ([MTLBuffer length] can be rounded up to whole pages); take sizes from
// the tensor's element count and dtype, never from the buffer's length.
int qcore_tensor_backend(const void* tensor);
// Backend-local ordinal for vendor APIs. Returns -1 for CPU/invalid tensors.
// The public Quidra device index remains available through qcore_tensor_device().
long long qcore_tensor_backend_device_index(const void* tensor);
uint64_t qcore_tensor_device_handle_const(const void* tensor);
uint64_t qcore_tensor_device_handle(void* tensor);
uint64_t qcore_tensor_device_offset_bytes(const void* tensor);
int qcore_device_activate(long long device);
// Backend-native execution queue/stream used by Core for same-device ordering.
// Metal returns a borrowed id<MTLCommandQueue> encoded as uint64_t.
// CUDA/HIP use their backend default stream, represented by native handle 0.
uint64_t qcore_device_queue_handle(long long device);

// Core's device command stream (additive to ABI version 1).
//
// Metal: Core keeps one open command buffer per device. It commits it when
// it holds enough work and before every host read or synchronization, and it
// waits only at those reads. Package kernels can append to it instead of
// committing (and waiting for) command buffers of their own:
//
//   // 0. Take mutable CPU pointers first (see "Commits and waits" below).
//   if (qcore_device_encode_begin(device)) {
//       // 1. Take every tensor handle and status slot. Core may work for
//       //    them now (copy-on-write detach, upload, new output storage,
//       //    status page clear), in program order, on encoders of its own.
//       in = qcore_tensor_device_handle_const(x);
//       out = qcore_tensor_output_handle(y);
//       ok = in && out && qcore_device_status_slot(device, &word, &at, &slot) == 0;
//       // 2. Open the scope: from here on Core does no work on the stream.
//       encoder = ok ? qcore_device_compute_encoder(device) : 0;
//       // 3. Set the pipeline state and bindings, and dispatch.
//       // 4. End scope and hold with exactly one qcore_metal_note_work.
//       qcore_metal_note_work(device, encoder ? 1 : 0, bytes);
//       // 5. Then defer (or release) the status slot, attach autograd.
//   } else {
//       // No Core stream: submit as before.
//   }
//
// qcore_device_encode_begin holds the device's stream for the calling thread
//   and returns 1, or returns 0 when the device has no Core stream (CUDA,
//   HIP, the fake backend, QUIDRA_METAL_STREAM=0) or the thread already
//   holds a stream. Other threads' work on the stream waits until the hold
//   ends, and Core commits nothing during it, so a buffer handle lent in the
//   hold is recorded as used by the command buffer the scope encodes into:
//   bind it only in that scope's work. Unlike a handle lent outside a hold,
//   it does not commit Core's batch.
// qcore_device_compute_encoder opens the encoder scope and returns Core's
//   open id<MTLComputeCommandEncoder> (serial dispatch, tracked resources).
//   Set the pipeline state and every binding the kernel uses, and dispatch;
//   never end it.
// qcore_metal_command_buffer opens a command-buffer scope instead: Core ends
//   its encoder and returns its open id<MTLCommandBuffer>, on which the
//   package creates, fills and ends encoders of its own (for example
//   concurrent ones); end them all before qcore_metal_note_work.
//   Without a hold, either call takes one first; handles taken before it
//   were lent outside a hold, which is correct but commits Core's batch at
//   every lend. Both return 0 when the device has no Core stream, when
//   Metal cannot create the encoder, and on a protocol violation (below).
// qcore_metal_note_work ends the scope and the hold, also after a failed
//   encode (with zero work) and when no scope was opened. `dispatches` and
//   `bytes` feed the commit policy. Returns 0, or 1 when the calling thread
//   holds no package encode on the device.
//
// Encoder ownership. A Metal encoder cannot save and restore its pipeline
// state and bindings, and an encoder Core ended would be released under the
// package. So while a scope is open, Core performs no encoding, upload,
// copy, fill, blit or copy-on-write detach on that device's stream. Inside
// the scope only lookups that need no Core work are allowed: tensor metadata,
// qcore_tensor_device_offset_bytes, qcore_tensor_device_handle_const of a
// tensor that is not a unified-memory view still shared with its transfer
// source, qcore_tensor_device_handle and qcore_tensor_output_handle of a
// dense tensor at offset 0 that covers the storage it alone owns,
// qcore_device_queue_handle, qcore_device_native_device,
// qcore_device_buffer_allocate, _handle and _release,
// qcore_device_defer_status, qcore_device_status_release,
// qcore_device_on_complete, qcore_tensor_mark_written and the counter calls.
// Anything else that would make Core work on the stream is a protocol
// violation: a handle that needs a copy (of a view of part of its storage),
// a copy-on-write detach, a relocation or storage of its own,
// qcore_device_status_slot, a custom autograd attach involving the device,
// qcore_device_encode_begin or a second scope (scopes neither nest nor
// change kind). Whether a call is refused depends only on the tensor's
// state, never on the buffer pool, the upload mode or GPU timing.
//
// Commits and waits. During the whole hold, from qcore_device_encode_begin
// (or the scope that took it) to qcore_metal_note_work, Core commits and
// waits for nothing on the held stream, so a request that needs a commit or
// a wait is a violation: qcore_device_flush, qcore_device_wait,
// qcore_device_status_wait, and qcore_tensor_cpu_data of a CPU tensor that
// is a unified-memory view device work may still read (Core would first
// wait for that work; refused whether or not it has finished). Take mutable
// CPU pointers before qcore_device_encode_begin.
//
// One device. A hold covers the device it was taken for: while a thread
// holds a stream, a call for another device that may need that device's
// stream is a violation (a tensor handle of a tensor on it, its queue
// handle, a scratch buffer on it or its handle, qcore_device_status_slot,
// _defer_status or _status_wait, qcore_device_on_complete, flush, wait, a
// custom autograd attach involving it). So a thread never waits for a
// second stream while it keeps one locked. Take what another device needs
// before the hold or after qcore_metal_note_work.
//
// Detection. Core detects each violation and does none of the requested
// work: the call fails with its failure value (handle 0, NULL pointer,
// nonzero status, -6 from an autograd attach). qcore_metal_note_work then
// discards the hold's open command buffer, so nothing encoded in it runs
// (Core work queued in it before the hold included), and stops the program
// with
//   Quidra runtime error[GPU_SCOPE] at FILE:L:C
// and the message <call and needed work> (status 101). FILE:L:C is where
// Core called the package code: the user's statement that made the extern
// call, or the backward() call
// whose custom autograd backward callback broke the protocol. Package code
// that Core runs outside any Quidra statement, a qcore_device_on_complete
// callback or a warm-up, names the program's root file without a line,
// and the process ends there at once (std::_Exit after flushing stdout and
// stderr) with the stream still held, so no other thread runs on without
// the discarded work; exit handlers, such as the report of deferred checks
// still pending, do not run then. On a thread that runs a task.all task,
// where the other tasks would run on during exit, the process also ends at
// once after the report, with the stream still held. Core never changes the
// state of an encoder the package holds, and never ends it.
//
// A hold or scope must end before the package code that began it returns:
// the extern call, or a callback Core runs (an autograd backward,
// qcore_device_on_complete, a warm-up). Core checks where control comes
// back: at the next Quidra statement of that thread, at the start of a
// task.all call and at the end of each of its tasks, when an autograd
// backward callback returns, after each completion callback and warm-up,
// and at exit. A hold found there is ended the same way (nothing encoded in
// its command buffer runs) and the program stops with GPU_SCOPE: at the
// statement that was running (the one that made the extern call), at the
// task.all call (a hold left open earlier in its statement, which its tasks
// would wait for), at the backward() call, or naming the root file after a completion
// callback, a warm-up or at exit; it ends at once there, as above, and on a
// task.all task's thread. Core work that thread asks for on the held stream
// before that point, for example later in the same statement, is refused
// where the hold refuses it (any work inside the scope, a commit or wait
// during the hold), and the program stops with GPU_SCOPE there; other work
// runs, and the check reports the hold. Other threads' work on the stream
// waits until the hold has been ended.
//
// Never wait, flush, synchronize or commit a command buffer of your own while
// holding the stream. Buffers bound in the scope stay alive until the work
// completes (Core's command buffer retains them). A queue handle taken during
// a hold makes the hold's end commit Core's open work, so command buffers the
// package commits on that queue afterwards run after it, and later
// synchronizations cover them. Only taking it during the hold does that: the
// hold's command buffer stays open after qcore_metal_note_work otherwise, so
// a command buffer the package commits on a queue handle it took before
// qcore_device_encode_begin (or cached from an earlier call) runs before the
// hold's work and reads what was there before it. Before committing command
// buffers of your own that use the hold's results, call
// qcore_device_queue_handle again during the hold (a lookup, also inside the
// scope). Packages that still commit their own command buffers on
// qcore_device_queue_handle keep working: lending a native handle outside a
// hold commits Core's open work first, and Core's synchronization covers the
// package's command buffers.
int qcore_device_encode_begin(long long device);
uint64_t qcore_device_compute_encoder(long long device);
uint64_t qcore_metal_command_buffer(long long device);
int qcore_metal_note_work(long long device, uint32_t dispatches,
                          uint64_t bytes);
// Commits queued work without waiting (a violation while the stream is held).
int qcore_device_flush(long long device);
// Commits and waits until all queued work on the device has completed,
// including command buffers package code committed on Core's queue, for a
// host read of package-owned device memory. Returns 0 on success, nonzero
// when a Core command buffer failed; that failure (like a deferred check)
// is still reported at the program's next synchronization point.
int qcore_device_wait(long long device);
// Runs callback(context) once every piece of work queued on the device so far
// has completed, including command buffers the package committed on Core's
// queue before the call, for example to release package-owned resources.
// The callback runs immediately on the calling thread when nothing is
// queued, except during the calling thread's package encode hold on the
// device (from qcore_device_encode_begin or a scope to
// qcore_metal_note_work): there it joins Core's open command buffer, which
// Core opens if needed, and runs once the hold's work has completed, never
// at once. On Metal it otherwise runs on a Core-owned serial queue (never
// inside a Metal completion handler), one callback at a time, and every
// synchronization point that waits for that work (a host read, gpu.sync,
// qcore_device_wait, program exit) returns only after the callback has run.
// A callback may use this stream ABI (package encodes, status slots,
// deferred checks, flush, wait); it must not wait for another completion
// callback.
// A wait for the callback's own device does not wait for that device's
// callbacks; a wait for another device does, so callbacks of two devices
// must not wait for each other's device.
// Metal completion handlers a package adds to its own command buffers must
// not call into Core's stream ABI: Core may hold the device stream while it
// waits for Metal to free a command buffer, which happens only after those
// handlers return. Other backends wait for the device, then run the callback.
int qcore_device_on_complete(long long device, void (*callback)(void*),
                             void* context);

// Write-only outputs, for a kernel that stores every element of its output.
// Pass the output as a mutable tensor parameter (any contents, for example
// tensor<T>(shape, gpu = n), which allocates without a fill) and call
// qcore_tensor_output_handle instead of qcore_tensor_device_handle: the
// tensor becomes uninitialized device storage of its dtype, shape and device
// (exclusive storage is reused, shared storage is left to its other owners),
// with no fill and no per-element bookkeeping, and the call returns its
// native handle (0: not eligible, use qcore_tensor_device_handle). Bind it
// at qcore_tensor_device_offset_bytes like any handle. After a successful
// encode, qcore_tensor_mark_written marks the whole storage written in
// O(1). Until then reads of the tensor fail as uninitialized, so after a
// failed encode its previous contents are gone and it must not be used.
// tensor.zeros keeps its meaning wherever zeros are needed. Eligible: an
// untracked, dense device tensor at offset 0 that covers its storage.
// qcore_tensor_mark_written returns 0, or nonzero (and changes nothing)
// unless the tensor is untracked, dense, at offset 0, covers its storage
// and owns it alone.
uint64_t qcore_tensor_output_handle(void* tensor);
int qcore_tensor_mark_written(void* tensor);

// Device status words for checked package kernels (for example a domain
// check of sqrt or log). qcore_device_status_slot hands out a zeroed 32-bit
// word (outside or in a hold, before the encoder scope opens; never inside
// it): bind `buffer` at `offset_bytes` and store a nonzero value from the
// kernel on failure. Once the kernel is
// encoded into Core's command buffer, or committed in the package's own,
// call exactly one of:
//   qcore_device_defer_status: the check is consumed at the next
//     synchronization point of the device (a host read such as .item() or
//     .cpu(), gpu.sync, a synchronized time sample, or program exit), which
//     fails with `message` and the statement that queued the work. No host
//     wait now (language.md: device-resident tensor checks).
//   qcore_device_status_wait: commits and waits now and returns the word in
//     *value (0 = success).
//   qcore_device_status_release: discards the slot (for example after a
//     failed encode).
// All return 0 on success.
int qcore_device_status_slot(long long device, uint64_t* buffer,
                             uint64_t* offset_bytes, uint64_t* slot);
int qcore_device_defer_status(long long device, uint64_t slot,
                              const char* message);
int qcore_device_status_wait(long long device, uint64_t slot,
                             uint32_t* value);
void qcore_device_status_release(long long device, uint64_t slot);

// Package warm-up. Registers `warmup` to prepare package kernels
// before their first use, for example to compile the package's Metal
// pipelines. Core calls warmup(device) once for every GPU device the program
// uses, on one Core-owned background thread: when the program first
// allocates on the device, or at registration for a device already in use.
// The compilation then overlaps the program's host work (data loading)
// instead of stalling its first GPU step, and a program that never uses a
// GPU runs none. Register from a static initializer of the package's native
// code, or at any later time; a function registered again is not run again.
// warmup runs concurrently with the program: the package's kernel cache must
// be thread-safe, and warmup must not encode, flush or wait for device work.
// Process exit stops after the running call; keep the state warmup uses in
// objects that exist before the program's first GPU allocation, or that are
// never destroyed. The runtime counter package_warmups counts finished calls.
void qcore_register_warmup(void (*warmup)(long long device));
// The borrowed backend-native device: id<MTLDevice> on Metal, 0 on other
// backends. For compiling package kernels (for example in a warm-up). It
// lends no queue; submit device work through Core's stream or
// qcore_device_queue_handle.
uint64_t qcore_device_native_device(long long device);

// Generic package scratch storage on a Core-owned device. The returned buffer
// token is opaque; packages may borrow only its backend-native handle and must
// release the token through Core. Its bytes are unspecified (not zeroed). On
// Metal it is shared storage that no queued GPU work uses when it is
// returned, so the package may write it through [buffer contents] before it
// encodes work that reads it. Tensor handles are different: queued Core work
// may still read or write a tensor's storage when its handle is lent, so host
// access to it through shared storage needs qcore_device_wait first.
void* qcore_device_buffer_allocate(long long device, uint64_t bytes);
uint64_t qcore_device_buffer_handle(const void* buffer);
void qcore_device_buffer_release(void* buffer);

// Process-wide execution policy is a domain-neutral mechanism shared by
// package backends. Packages decide what the policy means for their algorithms.
enum qcore_execution_policy {
    QCORE_EXECUTION_FAST = 0,
    QCORE_EXECUTION_DETERMINISTIC = 1
};
void qcore_execution_policy_set(int policy);
int qcore_execution_policy_get(void);

// Compatibility query for ABI-v1 packages. New packages should use
// qcore_execution_policy_get().
int qcore_execution_is_deterministic(void);

// region: parallel-abi
// Deterministic CPU parallelism for package-native kernels. Core owns the
// threads (Grand Central Dispatch on Apple platforms, a persistent pool
// elsewhere); packages never create their own.
//
// qcore_parallel_for splits [begin, end) into the fixed chunks
//   [begin + k * grain, min(begin + (k + 1) * grain, end)),  k = 0, 1, ...
// and calls body exactly once per chunk. The chunks depend only on begin, end
// and grain, never on the thread count. Chunks may run concurrently, in any
// order and on any thread, so a body must write only the outputs of its own
// range and compute each of them exactly as a serial loop would. Under that
// contract the results are bitwise identical for every thread count,
// including 1. A reduction may be split only into a fixed number of chunks
// whose partial results the caller then combines in a fixed order.
//
// Every chunk runs in ascending order on the calling thread, with no thread
// hand-off, when the range is a single chunk, when the effective thread count
// is 1 (QUIDRA_CPU_THREADS=1), when the call is nested inside another
// qcore_parallel_for body, or when it is made inside a task.all operation.
//
// Bodies on other threads run under the calling thread's floating-point
// environment (rounding mode, and flush-to-zero/denormal controls where the
// platform's fenv_t carries them, as on arm64 and x86-64), and the exception
// flags they raise are raised on the calling thread before the call returns.
// A body that changes the floating-point environment must restore it before
// returning. Bodies may run on threads with a smaller stack than the caller's
// (512 KiB for Grand Central Dispatch workers): keep large scratch buffers off
// the stack, for example by allocating them before the call and passing them
// through context.
//
// With one thread every chunk runs on the caller, so a body that overflows a
// helper's stack can pass at QUIDRA_CPU_THREADS=1 and fail only above it.
// Test package kernels both at 1 and with more than one thread.
//
// The body returns 0 on success or a positive package-defined error code.
// Once a chunk fails, chunks with a higher index that have not started are
// skipped; every chunk with a lower index still runs. The call returns 0 when
// every chunk succeeded and otherwise the result of the failing chunk with the
// lowest index, so the reported failure depends neither on the thread count
// nor on scheduling. Outputs are unspecified after a failure. A C++ exception
// that escapes a body is caught and reported as
// QCORE_PARALLEL_CALLBACK_EXCEPTION for that chunk; it is never rethrown.
// Negative results are reserved for Core: a body that returns any negative
// value fails its chunk with QCORE_PARALLEL_INVALID_BODY_RESULT, so a body
// result can never be mistaken for another status below.
//
// Inside a body. A body may call only the qcore_* functions that read data
// without changing Core state:
//   qcore_parallel_for (it then runs serially), qcore_parallel_thread_count,
//   qcore_native_abi_version, qcore_execution_policy_get,
//   qcore_execution_is_deterministic, and the tensor metadata queries
//   qcore_tensor_dtype, qcore_tensor_device, qcore_tensor_rank,
//   qcore_tensor_extent, qcore_tensor_element_count,
//   qcore_tensor_is_contiguous, qcore_tensor_backend,
//   qcore_tensor_backend_device_index and qcore_tensor_device_offset_bytes.
// Every other qcore_* function may allocate, copy, detach, synchronize,
// encode, register or record, which races on Core state when chunks run
// concurrently. Called from a body, at every thread count (1 included), such
// a function stops the program with runtime error PARALLEL_BODY (exit status
// 101) before it does anything. That includes the data accessors
// (qcore_tensor_cpu_data, qcore_tensor_device_handle and their _const forms):
// obtain data pointers and handles before the call and pass them through
// context.
//
// QUIDRA_CPU_THREADS=N (an integer from 1 to 256) limits each call to N
// threads including the caller; a value above the hardware thread count is
// reduced to it. Unset or empty means the hardware thread count. The variable
// is read and validated by the first qcore_parallel_for or
// qcore_parallel_thread_count call in the process, wherever that call is made
// (at top level, nested, or inside task.all); any other value stops the
// program there with runtime error CPU_THREADS (exit status 101). A program
// that must not stop part way, such as a benchmark harness before a timed run,
// calls qcore_parallel_thread_count() first to have the value checked up
// front. The variable does not limit task.all, which starts its own threads.
typedef int (*qcore_parallel_body_fn)(
    uint64_t begin, uint64_t end, void* context);

enum qcore_parallel_status {
    QCORE_PARALLEL_OK = 0,
    // body is NULL, grain is 0, or begin > end. No chunk ran.
    QCORE_PARALLEL_INVALID_ARGUMENT = -1,
    // A body let a C++ exception escape.
    QCORE_PARALLEL_CALLBACK_EXCEPTION = -2,
    // A body returned a negative value, which only Core may use.
    QCORE_PARALLEL_INVALID_BODY_RESULT = -3
};

int qcore_parallel_for(
    uint64_t begin,
    uint64_t end,
    uint64_t grain,
    qcore_parallel_body_fn body,
    void* context);

// Number of threads, including the caller, that a qcore_parallel_for issued
// from the calling context may use: QUIDRA_CPU_THREADS when it is set (reduced
// to the hardware thread count), the hardware thread count otherwise, and 1
// inside a qcore_parallel_for body or a task.all operation. It is an upper
// bound; a loaded system may run a call on fewer threads, which never changes
// its results. A package may use it to choose a grain; a partition derived
// from it is valid only over independent outputs, never for a reduction.
uint64_t qcore_parallel_thread_count(void);
// endregion: parallel-abi

// Opaque tensor handles are borrowed from Quidra extern parameters. Native
// packages must not depend on Core's private TensorValue/TensorStorage layout.
// These accessors expose contiguous CPU storage only. The mutable accessor
// returns NULL for tracked tensors so native mutation cannot bypass autograd,
// and applies Core's generic copy-on-write boundary before exposing storage.
// Pointers are valid for the duration of the extern call; like device handles
// above, a borrow may first give the tensor memory of its own. The mutable
// accessor of a unified-memory view that device work may still read first
// waits for that work, which a package encode hold refuses (see the stream
// section): take mutable CPU pointers before qcore_device_encode_begin.
const void* qcore_tensor_cpu_data_const(const void* tensor);
void* qcore_tensor_cpu_data(void* tensor);

// A package-owned first-order backward callback. Core owns every tensor
// allocation and lifetime; callbacks receive only call-scoped opaque borrows.
// gradient_inputs has one writable tensor for each differentiable input,
// contiguous at offset 0 on that input's device. gradient_output is
// contiguous at offset 0 on the device of the forward output: callbacks take
// no strides, so a GPU gradient that reaches the node as a view (for example
// through a transpose) is materialized densely before the call.
typedef int (*qcore_autograd_backward_fn)(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size);

// Optional higher-order callback used only by backward(track = true).
// differentiable_inputs are graph-preserving borrows of the original custom-op
// inputs; saved_tensors are graph-free value snapshots. The callback must write
// every gradient_inputs value and attach autograd provenance to each one (for
// example with qcore_tensor_attach_custom_autograd_ex). Core rejects a tracked
// callback that returns a graphless gradient.
// It runs for CPU and GPU graphs alike. A graph never mixes devices: on a GPU,
// gradient_output, differentiable_inputs and gradient_inputs are contiguous
// at device offset 0 on the node's device (a strided value is materialized
// first; the graph is kept), and the provenance the callback attaches stays
// on that device.
typedef int (*qcore_autograd_backward_tracked_fn)(
    const void* const* differentiable_inputs,
    uint64_t differentiable_input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size);

// Full package-owned differentiable-operation attachment. Saved tensors are
// retained as value snapshots for the graph lifetime and may differ in
// dtype/shape/device from differentiable inputs. backward_tracked is optional.
// Returns 0 on success (also when no input is tracked, which attaches
// nothing) and a negative value otherwise: -1 a missing argument, -2 an
// output without storage or already tracked, -3 an output dtype other than
// float32/float64, -4 an input whose dtype or device differs from the
// output's, -5 a missing saved tensor or one without storage, -6 an attach
// refused because the calling thread holds a device stream for a package
// encode (inside the encoder scope of a device one of the tensors lives on,
// or for another device than the hold's: a protocol violation, see the
// stream section above), -9 an internal failure.
//
// Saved tensors reach the callback dense. Core may keep a saved tensor that
// densely covers its storage by sharing that storage copy-on-write instead
// of copying it (other views are always copied). Every later write through
// Core detaches first: eager Quidra writes, and qcore_tensor_cpu_data or
// qcore_tensor_device_handle called after the attach. A mutable pointer or
// handle obtained before the attach is not detached, so a package must not
// write a saved tensor through one after attaching; finish such writes
// before the attach call, or save a separate tensor.
int qcore_tensor_attach_custom_autograd_with_saved_ex(
    void* output,
    const void* const* inputs,
    uint64_t input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    qcore_autograd_backward_fn backward,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const void* metadata,
    uint64_t metadata_size);

// Backward-compatible explicit-saved form without higher-order support.
int qcore_tensor_attach_custom_autograd_with_saved(
    void* output,
    const void* const* inputs,
    uint64_t input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    qcore_autograd_backward_fn backward,
    const void* metadata,
    uint64_t metadata_size);

// Convenience form that snapshots the differentiable inputs as saved tensors
// and optionally supplies a tracked backward callback.
int qcore_tensor_attach_custom_autograd_ex(
    void* output,
    const void* const* inputs,
    uint64_t input_count,
    qcore_autograd_backward_fn backward,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const void* metadata,
    uint64_t metadata_size);

// Masked first-order backward (additive ABI). Core computes which inputs
// need a gradient: an input needs one when a selected backward target is
// reachable through it. needed[i] is 1 for those and 0 otherwise, and
// gradient_inputs[i] is NULL exactly where needed[i] is 0. The callback runs
// only when at least one input needs a gradient, and its unrequested
// gradients are never computed. backward(track = true) still uses the
// tracked callback with full requests, and so does every backward with the
// rollback switch QUIDRA_AUTOGRAD_PRUNE=off, which marks every input needed.
//
// Write contract. fully_written[i] starts at 0, and the callback sets it to 1
// for each requested gradient it wrote completely. Whether Core may skip the
// zero fill of gradient i is decided before the call, so it comes from the
// declaration made at attach time (full_writes below), never from this flag:
//   - Gradient i not declared: it is zero-filled before every call, as for
//     qcore_autograd_backward_fn, so partial or accumulating writes stay
//     valid. fully_written[i] is informational.
//   - Gradient i declared: it is a write-only output (as with
//     qcore_tensor_output_handle). Its contents are unspecified when the
//     callback starts, because Core skips the zero fill wherever write-only
//     outputs do (today Metal and the fake test GPU;
//     QUIDRA_GPU_ZERO_FILL=always restores the fill). It is still borrowed
//     like any gradient (qcore_tensor_cpu_data, qcore_tensor_device_handle),
//     and the callback must store every element. After a call that returns
//     0, fully_written[i] must be 1 wherever gradient_inputs[i] was
//     requested; otherwise the backward fails before the gradient is used.
typedef int (*qcore_autograd_backward_masked_fn)(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    const uint8_t* needed,
    uint8_t* fully_written,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size);

// Saved-tensor attachment with a masked first-order backward callback.
// Arguments and return values are those of
// qcore_tensor_attach_custom_autograd_with_saved_ex, plus full_writes: NULL,
// or input_count bytes where full_writes[i] != 0 promises that the callback
// writes every element of gradient_inputs[i] whenever it is requested (see
// the write contract above). Core copies the bytes; the array is borrowed
// only for the duration of the call.
int qcore_tensor_attach_custom_autograd_masked(
    void* output,
    const void* const* inputs,
    uint64_t input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    qcore_autograd_backward_masked_fn backward,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const uint8_t* full_writes,
    const void* metadata,
    uint64_t metadata_size);

// Legacy first-order convenience form.
int qcore_tensor_attach_custom_autograd(
    void* output,
    const void* const* inputs,
    uint64_t input_count,
    qcore_autograd_backward_fn backward,
    const void* metadata,
    uint64_t metadata_size);

// Runtime counters (observation only; QUIDRA_COUNTERS=<file> writes one JSON
// record per program step). A package reports that its fast path refused an
// input together with the status it returned, so refusals show up per
// owner/function/status instead of silently taking a slower path.
void qcore_counter_note_refusal(const char* owner, const char* function,
                                int status);
// Ends one program step of the QUIDRA_COUNTERS report. Steps also end at
// every backward() unless QUIDRA_COUNTERS_STEP=none.
void qcore_counters_mark(const char* label);
// Test hook: process total of the named counter, 0 for an unknown name or
// when counting is off.
uint64_t qcore_counter_value(const char* name);

#ifdef __cplusplus
}
#endif
