#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

mkdir -p "$TMP/packages/native_pkg/native"

cat > "$TMP/packages/native_pkg/main.qui" <<'QUI'
extern int64 native_answer() = "qtest_native_answer"
extern int64 tensor_probe(
    const tensor<real32> &value
) = "qtest_tensor_probe"
extern int32 scale3_native(
    const tensor<real32> &input,
    tensor<real32> &output
) = "qtest_scale3"
extern int32 scale_saved_native(
    const tensor<real32> &input,
    const tensor<real32> &factor,
    tensor<real32> &output
) = "qtest_scale_saved"
extern int32 square_native(
    const tensor<real32> &input,
    tensor<real32> &output
) = "qtest_square"
extern int32 increment_first_native(
    tensor<real32> &value
) = "qtest_increment_first"
extern int32 mul_masked_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qtest_mul_masked"
extern int32 mul_full_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qtest_mul_full"
extern int32 mul_masked_partial_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qtest_mul_masked_partial"
extern int32 mul_masked_unreported_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qtest_mul_masked_unreported"
extern int32 mul_masked_undeclared_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qtest_mul_masked_undeclared"
extern int64 masked_last_native() = "qtest_masked_last"
extern int64 masked_calls_native() = "qtest_masked_calls"

tensor<real32> mul_masked(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape())
    int32 status = mul_masked_native(&left, &right, &output)
    if status != int32(0)
        error("masked custom autograd registration failed")
    return output

// mul_masked's callback without the full-write declaration.
tensor<real32> mul_masked_undeclared(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape())
    int32 status = mul_masked_undeclared_native(&left, &right, &output)
    if status != int32(0)
        error("masked custom autograd registration failed")
    return output

tensor<real32> mul_masked_partial(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape())
    int32 status = mul_masked_partial_native(&left, &right, &output)
    if status != int32(0)
        error("masked custom autograd registration failed")
    return output

tensor<real32> mul_masked_unreported(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape())
    int32 status = mul_masked_unreported_native(&left, &right, &output)
    if status != int32(0)
        error("masked custom autograd registration failed")
    return output

tensor<real32> mul_full(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape())
    int32 status = mul_full_native(&left, &right, &output)
    if status != int32(0)
        error("full-request custom autograd registration failed")
    return output

int masked_last()
    return int(masked_last_native())

int masked_calls()
    return int(masked_calls_native())
extern int32 square_any_native(
    const tensor<real32> &input,
    tensor<real32> &output
) = "qtest_square_any"

tensor<real32> scale3(tensor<real32> input)
    tensor<real32> output = tensor.zeros<real32>(input.shape())
    int32 status = scale3_native(&input, &output)
    if status != int32(0)
        error("native custom autograd registration failed")
    return output

tensor<real32> scale_saved(
    tensor<real32> input,
    tensor<real32> factor
)
    tensor<real32> output = tensor.zeros<real32>(input.shape())
    int32 status = scale_saved_native(&input, &factor, &output)
    if status != int32(0)
        error("native custom autograd saved-tensor registration failed")
    return output

tensor<real32> square(tensor<real32> input)
    tensor<real32> output = tensor.zeros<real32>(input.shape())
    int32 status = square_native(&input, &output)
    if status != int32(0)
        error("native tracked custom autograd registration failed")
    return output

// square() on the CPU or on the fake test GPU, whose device handle is host
// memory: exercises custom native nodes and their tracked callbacks on gpu(n).
tensor<real32> square_any(tensor<real32> input)
    tensor<real32> output = tensor.zeros<real32>(input.shape())
    if input.device() >= 0
        output = output.gpu(nat(input.device()))
    int32 status = square_any_native(&input, &output)
    if status != int32(0)
        error("native device custom autograd registration failed")
    return output

int32 increment_first(tensor<real32> &value)
    return increment_first_native(&value)

extern int32 stream_unavailable_native(tensor<real32> &value) = "qtest_stream_unavailable"

int32 stream_unavailable(tensor<real32> value)
    return stream_unavailable_native(&value)

int answer()
    return int(native_answer())

int probe()
    tensor<real32> value = tensor.ones<real32>([2])
    return int(tensor_probe(&value))
QUI

cat > "$TMP/packages/native_pkg/quidra.package" <<'MANIFEST'
name = native_pkg
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST

cat > "$TMP/packages/native_pkg/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

static thread_local long long qtest_tls_answer = 42;

extern "C" long long qtest_native_answer() {
    return qcore_native_abi_version() == QUIDRA_NATIVE_ABI_VERSION
        ? qtest_tls_answer
        : -1;
}

static int qtest_scale3_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 1;
    if (qcore_tensor_dtype(saved_tensors[0]) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(gradient_output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(gradient_inputs[0]) != QCORE_DTYPE_FLOAT32)
        return 2;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(gradient_output) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count)
        return 3;
    const auto* gradient =
        static_cast<const float*>(qcore_tensor_cpu_data_const(gradient_output));
    auto* input_gradient =
        static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    if (!gradient || !input_gradient) return 4;
    for (uint64_t index = 0; index < count; ++index)
        input_gradient[index] = gradient[index] * 3.0F;
    return 0;
}

extern "C" int32_t qtest_scale3(const void* input, void* output) {
    if (!input || !output ||
        qcore_tensor_dtype(input) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_backend(input) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 10;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(output) != count) return 11;
    const auto* source =
        static_cast<const float*>(qcore_tensor_cpu_data_const(input));
    auto* destination =
        static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!source || !destination) return 12;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * 3.0F;
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd(
        output, inputs, 1, qtest_scale3_backward, nullptr, 0);
}

static int qtest_scale_saved_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 21;
    const auto* factor =
        static_cast<const float*>(qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* gradient =
        static_cast<const float*>(qcore_tensor_cpu_data_const(gradient_output));
    auto* input_gradient =
        static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    if (!factor || !gradient || !input_gradient) return 22;
    const auto count = qcore_tensor_element_count(gradient_output);
    if (qcore_tensor_element_count(saved_tensors[0]) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count)
        return 23;
    for (uint64_t index = 0; index < count; ++index)
        input_gradient[index] = gradient[index] * factor[index];
    return 0;
}

extern "C" int32_t qtest_scale_saved(
    const void* input, const void* factor, void* output) {
    if (!input || !factor || !output ||
        qcore_tensor_dtype(input) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(factor) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_backend(input) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(factor) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 20;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(factor) != count ||
        qcore_tensor_element_count(output) != count)
        return 24;
    const auto* source =
        static_cast<const float*>(qcore_tensor_cpu_data_const(input));
    const auto* factor_data =
        static_cast<const float*>(qcore_tensor_cpu_data_const(factor));
    auto* destination =
        static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!source || !factor_data || !destination) return 25;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * factor_data[index];

    const void* inputs[] = {input};
    const void* saved[] = {factor};
    return qcore_tensor_attach_custom_autograd_with_saved(
        output, inputs, 1, saved, 1,
        qtest_scale_saved_backward, nullptr, 0);
}

static int qtest_mul2_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 2 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 2)
        return 30;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(saved_tensors[1]) != count ||
        qcore_tensor_element_count(gradient_output) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count ||
        qcore_tensor_element_count(gradient_inputs[1]) != count)
        return 31;
    const auto* x = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* g = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[1]));
    const auto* h = static_cast<const float*>(
        qcore_tensor_cpu_data_const(gradient_output));
    auto* dx = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    auto* dg = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[1]));
    if (!x || !g || !h || !dx || !dg) return 32;
    for (uint64_t index = 0; index < count; ++index) {
        dx[index] = 2.0F * g[index] * h[index];
        dg[index] = 2.0F * x[index] * h[index];
    }
    return 0;
}

static int qtest_square_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 40;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(gradient_output) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count)
        return 41;
    const auto* x = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* g = static_cast<const float*>(
        qcore_tensor_cpu_data_const(gradient_output));
    auto* dx = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    if (!x || !g || !dx) return 42;
    for (uint64_t index = 0; index < count; ++index)
        dx[index] = 2.0F * x[index] * g[index];
    return 0;
}

static int qtest_square_backward_tracked(
    const void* const* differentiable_inputs,
    uint64_t differentiable_input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size) {
    if (!differentiable_inputs || differentiable_input_count != 1)
        return 43;
    const int status = qtest_square_backward(
        saved_tensors, saved_tensor_count, gradient_output,
        gradient_inputs, gradient_input_count, metadata, metadata_size);
    if (status != 0) return status;
    const void* derivative_inputs[] = {
        differentiable_inputs[0], gradient_output
    };
    return qcore_tensor_attach_custom_autograd_ex(
        gradient_inputs[0], derivative_inputs, 2,
        qtest_mul2_backward, nullptr, nullptr, 0);
}

extern "C" int32_t qtest_square(const void* input, void* output) {
    if (!input || !output ||
        qcore_tensor_dtype(input) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_backend(input) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 50;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(output) != count) return 51;
    const auto* source =
        static_cast<const float*>(qcore_tensor_cpu_data_const(input));
    auto* destination =
        static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!source || !destination) return 52;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * source[index];
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd_ex(
        output, inputs, 1, qtest_square_backward,
        qtest_square_backward_tracked, nullptr, 0);
}

// Masked backward: records the mask, checks that exactly the needed
// gradients are requested and that the undeclared ones arrive zero-filled
// (the metadata is the attach-time full_writes declaration; declared ones
// are write-only outputs with unspecified contents), and writes them.
static long long qtest_masked_last_mask = -1;
static long long qtest_masked_call_count = 0;

static int qtest_mul_masked_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    const uint8_t* needed,
    uint8_t* fully_written,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size) {
    if (!saved_tensors || saved_tensor_count != 2 || !gradient_output ||
        !gradient_inputs || !needed || !fully_written ||
        gradient_input_count != 2 || !metadata || metadata_size != 2)
        return 80;
    const auto* declared = static_cast<const uint8_t*>(metadata);
    ++qtest_masked_call_count;
    qtest_masked_last_mask = (needed[0] ? 1 : 0) + (needed[1] ? 2 : 0);
    const auto count = qcore_tensor_element_count(gradient_output);
    const auto* x = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* y = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[1]));
    const auto* g = static_cast<const float*>(
        qcore_tensor_cpu_data_const(gradient_output));
    if (!x || !y || !g) return 81;
    for (uint64_t input = 0; input < 2; ++input) {
        if ((gradient_inputs[input] != nullptr) != (needed[input] != 0))
            return 82;
        if (fully_written[input] != 0) return 83;
        if (!needed[input]) continue;
        auto* out = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[input]));
        if (!out || qcore_tensor_element_count(gradient_inputs[input]) != count)
            return 84;
        const float* other = input == 0 ? y : x;
        for (uint64_t index = 0; index < count; ++index) {
            if (!declared[input] && out[index] != 0.0F) return 85;
            out[index] = g[index] * other[index];
        }
        fully_written[input] = 1;
    }
    return 0;
}

static int32_t qtest_mul_forward(
    const void* left, const void* right, void* output, float scale) {
    if (!left || !right || !output ||
        qcore_tensor_backend(left) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(right) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 90;
    const auto count = qcore_tensor_element_count(left);
    if (qcore_tensor_element_count(right) != count ||
        qcore_tensor_element_count(output) != count)
        return 91;
    const auto* a = static_cast<const float*>(qcore_tensor_cpu_data_const(left));
    const auto* b = static_cast<const float*>(qcore_tensor_cpu_data_const(right));
    auto* out = static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!a || !b || !out) return 92;
    for (uint64_t index = 0; index < count; ++index)
        out[index] = scale * a[index] * b[index];
    return 0;
}

extern "C" int32_t qtest_mul_masked(
    const void* left, const void* right, void* output) {
    const int32_t status = qtest_mul_forward(left, right, output, 1.0F);
    if (status != 0) return status;
    const void* inputs[] = {left, right};
    const uint8_t full_writes[] = {1, 1};
    return qcore_tensor_attach_custom_autograd_masked(
        output, inputs, 2, inputs, 2, qtest_mul_masked_backward,
        nullptr, full_writes, full_writes, 2);
}

extern "C" int32_t qtest_mul_masked_undeclared(
    const void* left, const void* right, void* output) {
    const int32_t status = qtest_mul_forward(left, right, output, 1.0F);
    if (status != 0) return status;
    const void* inputs[] = {left, right};
    const uint8_t declared[] = {0, 0};
    return qcore_tensor_attach_custom_autograd_masked(
        output, inputs, 2, inputs, 2, qtest_mul_masked_backward,
        nullptr, nullptr, declared, 2);
}

// Undeclared partial writer: writes only element 0 of each requested
// gradient, relies on the zero fill for the rest and reports no full write.
static int qtest_mul_masked_partial_backward(
    const void* const* saved_tensors, uint64_t, const void* gradient_output,
    void* const* gradient_inputs, const uint8_t* needed, uint8_t*,
    uint64_t gradient_input_count, const void*, uint64_t) {
    if (!saved_tensors || !gradient_output || !gradient_inputs || !needed ||
        gradient_input_count != 2)
        return 86;
    const auto* g = static_cast<const float*>(
        qcore_tensor_cpu_data_const(gradient_output));
    if (!g) return 87;
    for (uint64_t input = 0; input < 2; ++input) {
        if (!needed[input]) continue;
        const auto* other = static_cast<const float*>(
            qcore_tensor_cpu_data_const(saved_tensors[1 - input]));
        auto* out = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[input]));
        if (!other || !out) return 88;
        out[0] = g[0] * other[0];
    }
    return 0;
}

extern "C" int32_t qtest_mul_masked_partial(
    const void* left, const void* right, void* output) {
    const int32_t status = qtest_mul_forward(left, right, output, 1.0F);
    if (status != 0) return status;
    const void* inputs[] = {left, right};
    return qcore_tensor_attach_custom_autograd_masked(
        output, inputs, 2, inputs, 2, qtest_mul_masked_partial_backward,
        nullptr, nullptr, nullptr, 0);
}

// Declares full writes of input 1 but never reports one: Core must refuse
// the result instead of trusting the declaration.
static int qtest_mul_masked_unreported_backward(
    const void* const* saved_tensors, uint64_t saved_tensor_count,
    const void* gradient_output, void* const* gradient_inputs,
    const uint8_t* needed, uint8_t* fully_written,
    uint64_t gradient_input_count, const void* metadata, uint64_t metadata_size) {
    const int status = qtest_mul_masked_backward(
        saved_tensors, saved_tensor_count, gradient_output, gradient_inputs,
        needed, fully_written, gradient_input_count, metadata, metadata_size);
    fully_written[1] = 0;
    return status;
}

extern "C" int32_t qtest_mul_masked_unreported(
    const void* left, const void* right, void* output) {
    const int32_t status = qtest_mul_forward(left, right, output, 1.0F);
    if (status != 0) return status;
    const void* inputs[] = {left, right};
    const uint8_t full_writes[] = {0, 1};
    return qcore_tensor_attach_custom_autograd_masked(
        output, inputs, 2, inputs, 2, qtest_mul_masked_unreported_backward,
        nullptr, full_writes, full_writes, 2);
}

// A full-request callback (qtest_mul2_backward requires both gradients)
// keeps receiving every gradient even when only one input needs it.
extern "C" int32_t qtest_mul_full(
    const void* left, const void* right, void* output) {
    const int32_t status = qtest_mul_forward(left, right, output, 2.0F);
    if (status != 0) return status;
    const void* inputs[] = {left, right};
    return qcore_tensor_attach_custom_autograd_with_saved(
        output, inputs, 2, inputs, 2, qtest_mul2_backward, nullptr, 0);
}

extern "C" long long qtest_masked_last() { return qtest_masked_last_mask; }
extern "C" long long qtest_masked_calls() { return qtest_masked_call_count; }

extern "C" int32_t qtest_increment_first(void* value) {
    if (!value || qcore_tensor_backend(value) != QCORE_BACKEND_CPU ||
        qcore_tensor_dtype(value) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_element_count(value) == 0)
        return 1;
    auto* data = static_cast<float*>(qcore_tensor_cpu_data(value));
    if (!data) return 2;
    data[0] += 1.0F;
    return 0;
}

extern "C" long long qtest_tensor_probe(const void* value) {
    if (!value) return -1;
    if (qcore_tensor_backend(value) != QCORE_BACKEND_CPU) return -2;
    if (qcore_tensor_backend_device_index(value) != -1) return -8;
    if (qcore_tensor_device(value) != -1) return -3;
    if (!qcore_tensor_is_contiguous(value)) return -4;
    if (qcore_tensor_device_handle_const(value) != 0) return -5;
    if (qcore_tensor_device_offset_bytes(value) != 0) return -6;
    if (qcore_device_queue_handle(-1) != 0) return -9;
    if (qcore_execution_is_deterministic() != 0) return -7;
    return 43;
}

// Element access on the CPU or on the fake test GPU (whose device handle is
// host memory). The native ABI has no strides: anything else is rejected.
static const float* qtest_any_read(const void* tensor) {
    if (!tensor || qcore_tensor_dtype(tensor) != QCORE_DTYPE_FLOAT32)
        return nullptr;
    const int backend = qcore_tensor_backend(tensor);
    if (backend == QCORE_BACKEND_CPU)
        return static_cast<const float*>(qcore_tensor_cpu_data_const(tensor));
    if (backend != QCORE_BACKEND_TEST || !qcore_tensor_is_contiguous(tensor))
        return nullptr;
    const auto handle = qcore_tensor_device_handle_const(tensor);
    if (!handle) return nullptr;
    return reinterpret_cast<const float*>(
        handle + qcore_tensor_device_offset_bytes(tensor));
}

static float* qtest_any_write(void* tensor) {
    if (!tensor || qcore_tensor_dtype(tensor) != QCORE_DTYPE_FLOAT32)
        return nullptr;
    const int backend = qcore_tensor_backend(tensor);
    if (backend == QCORE_BACKEND_CPU)
        return static_cast<float*>(qcore_tensor_cpu_data(tensor));
    if (backend != QCORE_BACKEND_TEST || !qcore_tensor_is_contiguous(tensor))
        return nullptr;
    const auto handle = qcore_tensor_device_handle(tensor);
    if (!handle) return nullptr;
    return reinterpret_cast<float*>(
        handle + qcore_tensor_device_offset_bytes(tensor));
}

static int qtest_any_mul2_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 2 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 2)
        return 60;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    const auto* x = qtest_any_read(saved_tensors[0]);
    const auto* g = qtest_any_read(saved_tensors[1]);
    const auto* h = qtest_any_read(gradient_output);
    auto* dx = qtest_any_write(gradient_inputs[0]);
    auto* dg = qtest_any_write(gradient_inputs[1]);
    if (!x || !g || !h || !dx || !dg) return 61;
    for (uint64_t index = 0; index < count; ++index) {
        dx[index] = 2.0F * g[index] * h[index];
        dg[index] = 2.0F * x[index] * h[index];
    }
    return 0;
}

static int qtest_any_square_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 62;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(gradient_output) != count) return 63;
    const auto* x = qtest_any_read(saved_tensors[0]);
    const auto* g = qtest_any_read(gradient_output);
    auto* dx = qtest_any_write(gradient_inputs[0]);
    if (!x || !g || !dx) return 64;
    for (uint64_t index = 0; index < count; ++index)
        dx[index] = 2.0F * x[index] * g[index];
    return 0;
}

static int qtest_any_square_backward_tracked(
    const void* const* differentiable_inputs,
    uint64_t differentiable_input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size) {
    if (!differentiable_inputs || differentiable_input_count != 1)
        return 65;
    const int status = qtest_any_square_backward(
        saved_tensors, saved_tensor_count, gradient_output,
        gradient_inputs, gradient_input_count, metadata, metadata_size);
    if (status != 0) return status;
    const void* derivative_inputs[] = {
        differentiable_inputs[0], gradient_output
    };
    return qcore_tensor_attach_custom_autograd_ex(
        gradient_inputs[0], derivative_inputs, 2,
        qtest_any_mul2_backward, nullptr, nullptr, 0);
}

// Core's device command stream exists only on Metal. On the fake test GPU a
// package hold and an encoder scope are unavailable (0), ending one reports
// that none is open, and handles and flushes work as before: the package
// submits its work itself.
extern "C" int32_t qtest_stream_unavailable(void* value) {
    const auto device = qcore_tensor_device(value);
    if (qcore_device_encode_begin(device) != 0) return 1;
    if (qcore_device_compute_encoder(device) != 0) return 2;
    if (qcore_metal_command_buffer(device) != 0) return 3;
    if (qcore_metal_note_work(device, 0, 0) == 0) return 4;
    if (qcore_tensor_device_handle(value) == 0) return 5;
    if (qcore_tensor_output_handle(value) == 0) return 6;
    if (qcore_device_flush(device) != 0) return 7;
    return 0;
}

extern "C" int32_t qtest_square_any(const void* input, void* output) {
    if (!input || !output ||
        qcore_tensor_backend(input) != qcore_tensor_backend(output))
        return 66;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(output) != count) return 67;
    const auto* source = qtest_any_read(input);
    auto* destination = qtest_any_write(output);
    if (!source || !destination) return 68;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * source[index];
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd_ex(
        output, inputs, 1, qtest_any_square_backward,
        qtest_any_square_backward_tracked, nullptr, 0);
}
CPP

cat > "$TMP/use.qui" <<'QUI'
import package = native_pkg
print(package.answer())
print(NL)
print(package.probe())
print(NL)
// Native writable access must preserve source-level independent-value semantics.
tensor<real32> mutation_source = tensor.ones<real32>([1])
tensor<real32> mutation_copy = mutation_source
print(package.increment_first(&mutation_source) == int32(0))
print(NL)
print(mutation_source[0].item() == real32(2))
print(NL)
print(mutation_copy[0].item() == real32(1))
print(NL)
tensor<real32> tracked = tensor.ones<real32>([]).track()
tensor<real32> scaled = package.scale3(tracked)
scaled.backward(&tracked)
print(tracked.grad.item())
print(NL)
tensor<real32> tracked_saved = tensor.ones<real32>([]).track()
tensor<real32> factor = tensor.ones<real32>([1]) * real32(4)
tensor<real32> scaled_saved = package.scale_saved(tracked_saved, factor)
print(scaled_saved.untrack().item())
print(NL)
// Backward must consume the forward-time saved value, not this later mutation.
factor[0] = real32(9)
scaled_saved.backward(&tracked_saved)
print(tracked_saved.grad.item())
print(NL)
// Saved tensors share dense storage copy-on-write: a native in-place write
// through qcore_tensor_cpu_data must detach instead of changing the save.
tensor<real32> native_saved = tensor.ones<real32>([]).track()
tensor<real32> native_factor = tensor.ones<real32>([1]) * real32(5)
tensor<real32> native_scaled = package.scale_saved(native_saved, native_factor)
print(package.increment_first(&native_factor) == int32(0) and native_factor[0].item() == real32(6))
print(NL)
native_scaled.backward(&native_saved)
print(native_saved.grad.item())
print(NL)

tensor<real32> nonlinear = (tensor.ones<real32>([]) * real32(3)).track()
tensor<real32> squared = package.square(nonlinear)
squared.backward(&nonlinear, track = true)
tensor<real32> first_grad = nonlinear.grad
print(first_grad.untrack().item() == real32(6))
print(NL)
nonlinear.clear_grad()
first_grad.backward(&nonlinear)
print(nonlinear.grad.untrack().item() == real32(2))
print(NL)
QUI

# Saved tensors are snapshots both when copied and when shared
# copy-on-write (QUIDRA_SAVED_TENSORS=cow); the output must not differ.
for saved_mode in copy cow; do
    set +e
    QUIDRA_SAVED_TENSORS="$saved_mode" QUIDRA_PACKAGE_PATH="$TMP/packages" \
        "$QUIDRA" "$TMP/use.qui" >"$TMP/use.out" 2>"$TMP/use.err"
    use_status=$?
    set -e
    if [[ "$use_status" -ne 0 ]]; then
        echo "native package execution ($saved_mode saves) failed with status $use_status" >&2
        cat "$TMP/use.err" >&2
        exit 1
    fi
    output="$(cat "$TMP/use.out")"
    expected_output="$(printf '42\n43\ntrue\ntrue\ntrue\n3.0\n4.0\n4.0\ntrue\n5.0\ntrue\ntrue')"
    if [[ "$output" != "$expected_output" ]]; then
        echo "unexpected native package output ($saved_mode saves):" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi
done

# Gradient-need pruning: a masked callback is asked only for the gradients
# that reach a selected target and is not called when none does; a
# full-request callback still receives every gradient.
cat > "$TMP/masked-use.qui" <<'QUI'
import package = native_pkg
tensor<real32> x = tensor.ones<real32>([2]) * real32(3)
tensor<real32> y = tensor.ones<real32>([2]) * real32(5)
tensor<real32> product = package.mul_masked(x.track(), y.track())
tensor<real32> loss = product.gather([0], []) + product.gather([1], [])
loss.backward(&x)
print(x.grad[0].item() == real32(5) and x.grad[1].item() == real32(5) and package.masked_last() == 1)
print(NL)
loss.backward(&y)
print(y.grad[1].item() == real32(3) and package.masked_last() == 2)
print(NL)
loss.backward(&x, &y)
print(x.grad[0].item() == real32(10) and y.grad[0].item() == real32(6) and package.masked_last() == 3)
print(NL)
tensor<real32> p = tensor.ones<real32>([2]) * real32(7)
tensor<real32> constant = package.mul_masked(p.track(), y)
(constant.gather([0], []) + constant.gather([1], [])).backward(&p)
print(p.grad[1].item() == real32(5) and package.masked_last() == 1)
print(NL)
int calls = package.masked_calls()
tensor<real32> w = tensor.ones<real32>([]) * real32(2)
tensor<real32> unrelated = package.mul_masked(x.track(), y.track())
tensor<real32> mixed = unrelated.gather([0], []) * real32(0) + w.track() * real32(3)
mixed.backward(&w)
print(w.grad.item() == real32(3) and package.masked_calls() == calls)
print(NL)
tensor<real32> q = tensor.ones<real32>([2]) * real32(4)
tensor<real32> full = package.mul_full(q.track(), y.track())
(full.gather([0], []) + full.gather([1], [])).backward(&q)
print(q.grad[0].item() == real32(10))
print(NL)
tensor<real32> r = tensor.ones<real32>([2]) * real32(2)
tensor<real32> partial = package.mul_masked_partial(r.track(), y)
(partial.gather([0], []) + partial.gather([1], [])).backward(&r)
print(r.grad[0].item() == real32(5) and r.grad[1].item() == real32(0))
print(NL)
tensor<real32> s = tensor.ones<real32>([2]) * real32(2)
tensor<real32> undeclared = package.mul_masked_unreported(s.track(), y)
(undeclared.gather([0], []) + undeclared.gather([1], [])).backward(&s)
print(s.grad[1].item() == real32(5))
print(NL)
// Declared full writes are write-only outputs; the gradients are bitwise
// those of the same callback without the declaration.
tensor<real32> left = tensor.zeros<real32>([3])
left[0] = real32(0.1)
left[1] = real32(-2.5)
left[2] = real32(3.7)
tensor<real32> right = tensor.zeros<real32>([3])
right[0] = real32(1.3)
right[1] = real32(0.7)
right[2] = real32(-0.9)
tensor<real32> weights = tensor.zeros<real32>([3])
weights[0] = real32(0.25)
weights[1] = real32(-1.5)
weights[2] = real32(2.125)
tensor<real32> a = left
tensor<real32> b = right
tensor<real32> declared = package.mul_masked(a.track(), b.track()) * weights
(declared.gather([0], []) + declared.gather([1], []) + declared.gather([2], [])).backward(&a, &b)
tensor<real32> c = left
tensor<real32> d = right
tensor<real32> plain = package.mul_masked_undeclared(c.track(), d.track()) * weights
(plain.gather([0], []) + plain.gather([1], []) + plain.gather([2], [])).backward(&c, &d)
print((a.grad == c.grad).all() and (b.grad == d.grad).all() and (a.grad == right * weights).all() and (b.grad == left * weights).all())
print(NL)
QUI
masked_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/masked-use.qui" 2>&1)"
if [[ "$masked_output" != "$(for _ in {1..9}; do echo true; done)" ]]; then
    echo "unexpected masked custom autograd output:" >&2
    printf '%s\n' "$masked_output" >&2
    exit 1
fi

# A declared full write that the callback does not report fails the backward
# (here the declared input 1 is requested).
cat > "$TMP/masked-unreported.qui" <<'QUI'
import package = native_pkg
tensor<real32> x = tensor.ones<real32>([2]) * real32(3)
tensor<real32> y = tensor.ones<real32>([2]) * real32(5)
tensor<real32> product = package.mul_masked_unreported(x, y.track())
(product.gather([0], []) + product.gather([1], [])).backward(&y)
print(y.grad[0].item())
print(NL)
QUI
set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/masked-unreported.qui" \
    >"$TMP/masked-unreported.out" 2>"$TMP/masked-unreported.err"
unreported_status=$?
set -e
if [[ "$unreported_status" -eq 0 ]] ||
   ! grep -q "did not report a complete write of gradient input 1 declared as fully written" \
        "$TMP/masked-unreported.err" ||
   [[ -s "$TMP/masked-unreported.out" ]]; then
    echo "an unreported declared full write was accepted (status $unreported_status):" >&2
    cat "$TMP/masked-unreported.out" "$TMP/masked-unreported.err" >&2
    exit 1
fi

# Gradient-need pruning skips the formulas of values that reach no target.
cat > "$TMP/autograd-stats.qui" <<'QUI'
tensor<real32> a = tensor.ones<real32>([])
tensor<real32> b = tensor.ones<real32>([])
tensor<real32> loss = a.track() * real32(2) + b.track() * real32(3)
loss.backward(&a)
print(a.grad.item())
print(NL)
QUI
stats_output="$(QUIDRA_TEST_AUTOGRAD_STATS=1 "$QUIDRA" "$TMP/autograd-stats.qui" 2>&1)"
if [[ "$stats_output" != "$(printf 'autograd stats: formulas 2 pruned 1\n2.0')" ]]; then
    echo "unexpected autograd pruning statistics:" >&2
    printf '%s\n' "$stats_output" >&2
    exit 1
fi
# QUIDRA_AUTOGRAD_PRUNE=off restores the full walk.
stats_output="$(QUIDRA_AUTOGRAD_PRUNE=off QUIDRA_TEST_AUTOGRAD_STATS=1 "$QUIDRA" "$TMP/autograd-stats.qui" 2>&1)"
if [[ "$stats_output" != "$(printf 'autograd stats: formulas 3 pruned 0\n2.0')" ]]; then
    echo "unexpected autograd statistics with pruning off:" >&2
    printf '%s\n' "$stats_output" >&2
    exit 1
fi

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" repl < "$TMP/use.qui" \
    >"$TMP/repl.out" 2>"$TMP/repl.err"
repl_status=$?
set -e
if [[ "$repl_status" -ne 0 ]]; then
    echo "native package REPL execution failed with status $repl_status" >&2
    cat "$TMP/repl.err" >&2
    exit 1
fi
repl_output="$(cat "$TMP/repl.out")"
if ! grep -q '^42$' <<< "$repl_output"; then
    echo "native package REPL output missing expected answer:" >&2
    printf '%s\n' "$repl_output" >&2
    cat "$TMP/repl.err" >&2
    exit 1
fi

# Custom native nodes on gpu(n) (test builds with the fake GPU backend only;
# its device handles are host memory): first order through a transposed
# gradient view, which the native ABI must receive dense, and the package's
# tracked callback for backward(track = true), both against the CPU.
if QUIDRA_TEST_FAKE_GPU_COUNT=1 "$QUIDRA" gpu 2>/dev/null | grep -Fq "backend: TEST"; then
    cat > "$TMP/device-use.qui" <<'QUI'
import package = native_pkg

tensor<real32> total(tensor<real32> value)
    tensor<real32> flat = value.reshape([6])
    tensor<real32> result = flat.gather([0], [])
    for index in range(1, 6)
        result = result + flat.gather([index], [])
    return result

tensor<real32> values(int seed)
    tensor<real32> result = tensor.zeros<real32>([2, 3])
    for row in range(2)
        for column in range(3)
            real32 cell = real32((row * 3 + column + seed) % 5)
            result[row, column] = cell + real32(0.5)
    return result

tensor<real32> place(tensor<real32> value, int device)
    if device >= 0
        return value.gpu(nat(device))
    return value

class Result
    tensor<real32> first
    tensor<real32> second

// loss = sum(square(x)^T * c): the gradient reaching the custom node is a
// transposed (strided) view.
Result derivatives(int device)
    tensor<real32> x = place(values(1), device).track()
    tensor<real32> c = place(values(2).transpose(0, 1).contiguous(), device)
    tensor<real32> loss = total(package.square_any(x).transpose(0, 1) * c)
    loss.backward(&x, track = true)
    tensor<real32> first = x.grad
    Result result
    result.first = first.untrack().cpu()
    x.clear_grad()
    total(first * place(values(3), device)).backward(&x)
    result.second = x.grad.untrack().cpu()
    return result

Result host = derivatives(-1)
Result device_result = derivatives(0)
print((host.first == device_result.first).all() and (host.second == device_result.second).all())
print(NL)
// d/dx sum(x^2 * c^T) = 2 x c^T; d/dx sum(2 x c^T p) = 2 c^T p
tensor<real32> expected_first = values(1) * values(2) * real32(2)
tensor<real32> expected_second = values(2) * values(3) * real32(2)
print((device_result.first == expected_first).all() and (device_result.second == expected_second).all())
print(NL)

// First order alone on the device through the same transposed gradient.
tensor<real32> y = place(values(1), 0).track()
total(package.square_any(y).transpose(0, 1) * place(values(2).transpose(0, 1).contiguous(), 0)).backward(&y)
print((y.grad.cpu() == expected_first).all())
print(NL)
// No Core command stream for package encodes on the fake GPU.
print(package.stream_unavailable(tensor.ones<real32>([4]).gpu(0)) == int32(0))
print(NL)
QUI
    set +e
    QUIDRA_TEST_FAKE_GPU_COUNT=1 QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/device-use.qui" \
        >"$TMP/device-use.out" 2>"$TMP/device-use.err"
    device_use_status=$?
    set -e
    device_use_output="$(cat "$TMP/device-use.out")"
    if [[ "$device_use_status" -ne 0 || "$device_use_output" != "$(printf 'true\ntrue\ntrue\ntrue')" ]]; then
        echo "native custom autograd on the fake GPU failed (status $device_use_status):" >&2
        printf '%s\n' "$device_use_output" >&2
        cat "$TMP/device-use.err" >&2
        exit 1
    fi
fi

HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR"
HOME="$HOME_DIR" "$QUIDRA" package install "$TMP/packages/native_pkg" --force >/dev/null
HOME="$HOME_DIR" "$QUIDRA" package-info native_pkg --json > "$TMP/info.json"
python3 - "$TMP/info.json" <<'PY'
import json
import sys
info = json.load(open(sys.argv[1]))
assert info["native_source"]["bridge"] == "native/bridge.cpp"
PY

cat > "$TMP/installed-use.qui" <<'QUI'
import package = native_pkg
print(package.answer())
print(NL)
print(package.probe())
print(NL)
QUI
set +e
HOME="$HOME_DIR" "$QUIDRA" "$TMP/installed-use.qui" \
    >"$TMP/installed-use.out" 2>"$TMP/installed-use.err"
installed_use_status=$?
set -e
installed_use_output="$(cat "$TMP/installed-use.out")"
if [[ "$installed_use_status" -ne 0 || "$installed_use_output" != "$(printf '42\n43')" ]]; then
    echo "installed native package execution mismatch (status $installed_use_status):" >&2
    printf '%s\n' "$installed_use_output" >&2
    cat "$TMP/installed-use.err" >&2
    exit 1
fi

mkdir -p "$TMP/packages/invalid_extension/compiler"
cat > "$TMP/packages/invalid_extension/main.qui" <<'QUI'
tensor<real32> identity(tensor<real32> value)
    return value
QUI
cat > "$TMP/packages/invalid_extension/quidra.package" <<'MANIFEST'
name = invalid_extension
version = 0.1.0
repository = https://example.invalid/invalid_extension
requires.quidra = >=0.5.0 <0.6.0
MANIFEST
cat > "$TMP/packages/invalid_extension/project.toml" <<'TOML'
[package]
name = "test-invalid-extension"
import = "invalid_extension"
display_name = "Invalid Extension Test"
version = "0.1.0"
repository = "https://example.invalid/invalid_extension"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1

[compiler.extension]
graph = "compiler/graph.toml"
TOML
cat > "$TMP/packages/invalid_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[operation.identity]
function = "identity"
traits = "pure,tensor"

[fusion.invalid]
operations = "identity,missing"
TOML
cat > "$TMP/invalid-extension-use.qui" <<'QUI'
import broken = invalid_extension

tensor<real32> value = broken.identity(tensor.ones<real32>([1]))
print(value[0].item())
print(NL)
QUI

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/invalid-extension-use.qui" --json \
    >"$TMP/invalid-extension.out" 2>&1
invalid_extension_status=$?
set -e
if [[ "$invalid_extension_status" -ne 1 ]]; then
    echo "invalid compiler extension descriptor unexpectedly passed" >&2
    cat "$TMP/invalid-extension.out" >&2
    exit 1
fi
grep -Fq "PACKAGE_COMPILER_EXTENSION" "$TMP/invalid-extension.out"
grep -Fq "references unknown operation 'missing'" "$TMP/invalid-extension.out"

cat > "$TMP/packages/invalid_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[operation.identity]
function = "identity"
traits = "pure,tensor"

[specialization.invalid]
operation = "identity"
replacement = "missing"
dtype = "real32"
TOML

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/invalid-extension-use.qui" --json \
    >"$TMP/invalid-conditional-extension.out" 2>&1
invalid_conditional_extension_status=$?
set -e
if [[ "$invalid_conditional_extension_status" -ne 1 ]]; then
    echo "invalid compiler conditional extension unexpectedly passed" >&2
    cat "$TMP/invalid-conditional-extension.out" >&2
    exit 1
fi
grep -Fq "PACKAGE_COMPILER_EXTENSION" "$TMP/invalid-conditional-extension.out"
grep -Fq "specialization 'invalid' references unknown replacement operation 'missing'" \
    "$TMP/invalid-conditional-extension.out"

cat > "$TMP/packages/invalid_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[execution_policy.fast]
function = "fast"

[operation.identity]
function = "identity"
traits = "pure,tensor"

[specialization.invalid_policy]
operation = "identity"
replacement = "identity"
policy = "deterministic"
TOML

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/invalid-extension-use.qui" --json \
    >"$TMP/invalid-policy-extension.out" 2>&1
invalid_policy_extension_status=$?
set -e
if [[ "$invalid_policy_extension_status" -ne 1 ]]; then
    echo "invalid compiler execution policy unexpectedly passed" >&2
    cat "$TMP/invalid-policy-extension.out" >&2
    exit 1
fi
grep -Fq "PACKAGE_COMPILER_EXTENSION" "$TMP/invalid-policy-extension.out"
grep -Fq "references unknown execution policy 'deterministic'" \
    "$TMP/invalid-policy-extension.out"

mkdir -p "$TMP/packages/policy_extension/compiler"
cat > "$TMP/packages/policy_extension/main.qui" <<'QUI'
void fast()
    return

void deterministic()
    return

void opaque()
    return

tensor<real32> portable(tensor<real32> value)
    return value

tensor<real32> fast_target(tensor<real32> value)
    return value

tensor<real32> deterministic_target(tensor<real32> value)
    return value
QUI
cat > "$TMP/packages/policy_extension/quidra.package" <<'MANIFEST'
name = policy_extension
version = 0.1.0
repository = https://example.invalid/policy_extension
requires.quidra = >=0.5.0 <0.6.0
MANIFEST
cat > "$TMP/packages/policy_extension/project.toml" <<'TOML'
[package]
name = "test-policy-extension"
import = "policy_extension"
display_name = "Policy Extension Test"
version = "0.1.0"
repository = "https://example.invalid/policy_extension"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1

[compiler.extension]
graph = "compiler/graph.toml"
TOML
cat > "$TMP/packages/policy_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[execution_policy.fast]
function = "fast"

[execution_policy.deterministic]
function = "deterministic"

[operation.portable]
function = "portable"
traits = "pure,tensor,differentiable,higher-order"

[operation.fast_target]
function = "fast_target"
traits = "pure,tensor,differentiable,higher-order,backend-target"

[operation.deterministic_target]
function = "deterministic_target"
traits = "pure,tensor,differentiable,higher-order,backend-target"

[backend.fast]
operation = "portable"
replacement = "fast_target"
policy = "fast"
dtype = "real32"
device = "cpu"
layout = "contiguous"
tracked = "false"

[backend.deterministic]
operation = "portable"
replacement = "deterministic_target"
policy = "deterministic"
dtype = "real32"
device = "cpu"
layout = "contiguous"
tracked = "false"
TOML
cat > "$TMP/policy-extension-use.qui" <<'QUI'
import policy = policy_extension

policy.fast()
tensor<real32> first = policy.portable(tensor.ones<real32>([1]))
policy.deterministic()
tensor<real32> second = policy.portable(tensor.ones<real32>([1]))
policy.fast()
policy.opaque()
tensor<real32> third = policy.portable(tensor.ones<real32>([1]))
print(first[0].item() == real32(1))
print(NL)
print(second[0].item() == real32(1))
print(NL)
print(third[0].item() == real32(1))
print(NL)
QUI
policy_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/policy-extension-use.qui")"
if [[ "$policy_output" != "$(printf 'true\ntrue\ntrue')" ]]; then
    echo "policy extension execution mismatch:" >&2
    printf '%s\n' "$policy_output" >&2
    exit 1
fi
policy_ir="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" ir "$TMP/policy-extension-use.qui")"
policy_entry="$(awk '/^function \$entry\(/,/^end$/' <<< "$policy_ir")"
policy_fast_count="$(grep -Fc "call policy.fast_target(" <<< "$policy_entry")"
policy_deterministic_count="$(grep -Fc "call policy.deterministic_target(" <<< "$policy_entry")"
policy_portable_count="$(grep -Fc "call policy.portable(" <<< "$policy_entry")"
if [[ "$policy_fast_count" -ne 1 || "$policy_deterministic_count" -ne 1 || "$policy_portable_count" -lt 1 ]]; then
    echo "policy extension IR mismatch: fast=$policy_fast_count deterministic=$policy_deterministic_count portable=$policy_portable_count" >&2
    printf '%s\n' "$policy_entry" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Linux" ]]; then
    mkdir -p "$TMP/fake-cuda/bin" "$TMP/fake-cuda/lib64"
    cat > "$TMP/fake-cuda/bin/nvcc" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
args=()
for argument in "$@"; do
    case "$argument" in
        -Xcompiler=-fPIC)
            args+=("-fPIC")
            ;;
        *.cu)
            args+=("-x" "c++" "$argument")
            ;;
        *)
            args+=("$argument")
            ;;
    esac
done
exec clang++ "${args[@]}"
SH
    chmod +x "$TMP/fake-cuda/bin/nvcc"
    printf '%s\n' 'extern "C" void qtest_cudart_stub() {}' |
        clang++ -shared -fPIC -x c++ - -o "$TMP/fake-cuda/lib64/libcudart.so"

    mkdir -p "$TMP/packages/cuda_pkg/native"
    cat > "$TMP/packages/cuda_pkg/main.qui" <<'QUI'
extern int64 cuda_answer() = "qtest_cuda_answer"

int answer()
    return int(cuda_answer())
QUI
    cat > "$TMP/packages/cuda_pkg/quidra.package" <<'MANIFEST'
name = cuda_pkg
version = 0.1.0
native.source.cuda = native/answer.cu
MANIFEST
    cat > "$TMP/packages/cuda_pkg/native/answer.cu" <<'CU'
#include <quidra/native_extension.h>

extern "C" long long qtest_cuda_answer() {
    return QUIDRA_NATIVE_ABI_VERSION == 1u ? 44 : -1;
}
CU
    cat > "$TMP/cuda-use.qui" <<'QUI'
import package = cuda_pkg
print(package.answer())
print(NL)
QUI

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    QUIDRA_NVCC="$TMP/fake-cuda/bin/nvcc" \
    QUIDRA_CUDA_HOME="$TMP/fake-cuda" \
    "$QUIDRA" "$TMP/cuda-use.qui" \
        >"$TMP/cuda-use.out" 2>"$TMP/cuda-use.err"
    cuda_status=$?
    set -e
    cuda_output="$(cat "$TMP/cuda-use.out")"
    if [[ "$cuda_status" -ne 0 || "$cuda_output" != "44" ]]; then
        echo "CUDA package execution mismatch (status $cuda_status):" >&2
        printf '%s\n' "$cuda_output" >&2
        cat "$TMP/cuda-use.err" >&2
        exit 1
    fi

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    QUIDRA_NVCC="$TMP/fake-cuda/bin/nvcc" \
    QUIDRA_CUDA_HOME="$TMP/fake-cuda" \
    "$QUIDRA" repl < "$TMP/cuda-use.qui" \
        >"$TMP/cuda-repl.out" 2>"$TMP/cuda-repl.err"
    cuda_repl_status=$?
    set -e
    cuda_repl_output="$(cat "$TMP/cuda-repl.out")"
    if [[ "$cuda_repl_status" -ne 0 ]] || ! grep -q '^44$' <<< "$cuda_repl_output"; then
        echo "CUDA package REPL mismatch (status $cuda_repl_status):" >&2
        printf '%s\n' "$cuda_repl_output" >&2
        cat "$TMP/cuda-repl.err" >&2
        exit 1
    fi
fi

if [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "x86_64" ]]; then
    mkdir -p "$TMP/packages/asm_pkg/native"
    cat > "$TMP/packages/asm_pkg/main.qui" <<'QUI'
extern int64 asm_answer() = "qtest_asm_answer"

int answer()
    return int(asm_answer())
QUI
    cat > "$TMP/packages/asm_pkg/quidra.package" <<'MANIFEST'
name = asm_pkg
version = 0.1.0
native.source.asm = native/answer.S
MANIFEST
    cat > "$TMP/packages/asm_pkg/native/answer.S" <<'ASM'
.text
.globl qtest_asm_answer
.type qtest_asm_answer,@function
qtest_asm_answer:
    mov $45, %rax
    ret
.section .note.GNU-stack,"",@progbits
ASM
    cat > "$TMP/asm-use.qui" <<'QUI'
import package = asm_pkg
print(package.answer())
print(NL)
QUI

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    "$QUIDRA" "$TMP/asm-use.qui" \
        >"$TMP/asm-use.out" 2>"$TMP/asm-use.err"
    asm_status=$?
    set -e
    asm_output="$(cat "$TMP/asm-use.out")"
    if [[ "$asm_status" -ne 0 || "$asm_output" != "45" ]]; then
        echo "assembly package execution mismatch (status $asm_status):" >&2
        printf '%s\n' "$asm_output" >&2
        cat "$TMP/asm-use.err" >&2
        exit 1
    fi

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    "$QUIDRA" repl < "$TMP/asm-use.qui" \
        >"$TMP/asm-repl.out" 2>"$TMP/asm-repl.err"
    asm_repl_status=$?
    set -e
    asm_repl_output="$(cat "$TMP/asm-repl.out")"
    if [[ "$asm_repl_status" -ne 0 ]] || ! grep -q '^45$' <<< "$asm_repl_output"; then
        echo "assembly package REPL mismatch (status $asm_repl_status):" >&2
        printf '%s\n' "$asm_repl_output" >&2
        cat "$TMP/asm-repl.err" >&2
        exit 1
    fi
fi

echo "package native tests: ok"
