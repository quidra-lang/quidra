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

// Generic package scratch storage on a Core-owned device. The returned buffer
// token is opaque; packages may borrow only its backend-native handle and must
// release the token through Core.
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

// Opaque tensor handles are borrowed from Quidra extern parameters. Native
// packages must not depend on Core's private TensorValue/TensorStorage layout.
// These accessors expose contiguous CPU storage only. The mutable accessor
// returns NULL for tracked tensors so native mutation cannot bypass autograd,
// and applies Core's generic copy-on-write boundary before exposing storage.
const void* qcore_tensor_cpu_data_const(const void* tensor);
void* qcore_tensor_cpu_data(void* tensor);

// A package-owned first-order backward callback. Core owns every tensor
// allocation and lifetime; callbacks receive only call-scoped opaque borrows.
// gradient_inputs has one writable tensor for each differentiable input.
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

// Legacy first-order convenience form.
int qcore_tensor_attach_custom_autograd(
    void* output,
    const void* const* inputs,
    uint64_t input_count,
    qcore_autograd_backward_fn backward,
    const void* metadata,
    uint64_t metadata_size);

#ifdef __cplusplus
}
#endif
