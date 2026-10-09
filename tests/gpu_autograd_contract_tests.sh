#!/usr/bin/env bash
# Device autograd, broadcast and native-handle contracts on the fake GPU
# backend: strided broadcasting against host-built gather indices, the device
# broadcast backward, copy-on-write saved tensors, native handle
# initialization checks, dense gradients and masked callbacks for custom
# native backward. Split from gpu_contract_tests.sh so that each stays within
# the ctest timeout.
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

cat > "$TMP/reduction_helpers.qui" <<'QUI'
int element_count(tensor<real32> value)
    int count = 1
    for extent in value.shape()
        count = count * int(extent)
    return count

tensor<real32> sum(tensor<real32> value)
    int count = element_count(value)
    if count == 0
        if value.device() >= 0
            return tensor.zeros<real32>([], gpu = nat(value.device()))
        return tensor.zeros<real32>([])
    tensor<real32> result = value.gather([0], [])
    for index in range(1, count)
        result = result + value.gather([index], [])
    return result

tensor<real32> mean(tensor<real32> value)
    int count = element_count(value)
    if count == 0
        error("test mean requires at least one element")
    real32 scale = real32(count)
    return sum(value) / scale
QUI

# This environment variable is recognized only when Quidra was compiled with
# QUIDRA_ENABLE_TEST_GPU_BACKEND. Production builds contain no fake backend.
export QUIDRA_TEST_FAKE_GPU_COUNT=2

# Broadcast, transposed, stepped and scalar arithmetic and their gradients
# read strided operands on the device. The test backend computes on the host,
# so every value must be bitwise the CPU one, with strided reads
# (QUIDRA_BROADCAST=strided) and with host-built gather indices
# (QUIDRA_BROADCAST=gather).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$QUIDRA" build "$SCRIPT_DIR/gpu_broadcast_bitwise.qui" -o "$TMP/broadcast-bitwise" >/dev/null
"$TMP/broadcast-bitwise" --device -1 >"$TMP/broadcast-cpu.out"
for variant in strided gather; do
    QUIDRA_BROADCAST="$variant" "$TMP/broadcast-bitwise" --device 0 \
        >"$TMP/broadcast-$variant.out"
    if ! cmp -s "$TMP/broadcast-cpu.out" "$TMP/broadcast-$variant.out"; then
        echo "fake-GPU $variant broadcast differs from the CPU" >&2
        diff "$TMP/broadcast-cpu.out" "$TMP/broadcast-$variant.out" | head -20 >&2
        exit 1
    fi
done
# An unrecognized switch value keeps the previous behaviour and says so.
QUIDRA_BROADCAST=off "$TMP/broadcast-bitwise" --device 0 \
    >"$TMP/broadcast-off.out" 2>"$TMP/broadcast-off.err"
if ! cmp -s "$TMP/broadcast-gather.out" "$TMP/broadcast-off.out" ||
   [[ "$(cat "$TMP/broadcast-off.err")" != "Quidra runtime warning: unrecognized QUIDRA_BROADCAST=off (expected strided or gather); using gather" ]]; then
    echo "QUIDRA_BROADCAST=off did not fall back to gather with a warning:" >&2
    cat "$TMP/broadcast-off.err" >&2
    exit 1
fi

# Shapes that cannot broadcast (different ranks, or two extents above 1) fail
# with the same diagnostic on the CPU and on the device in both modes, with
# untracked and tracked operands.
cat > "$TMP/broadcast-errors.qui" <<'QUI'
cli args
    int device = option(default = -1)
    int case = option(default = 0)
    bool tracked = option(default = false)

tensor<real32> place(tensor<real32> value, int device)
    if device >= 0
        return value.gpu(nat(device))
    return value

tensor<real32> left = place(tensor.ones<real32>([2, 3]), args.device)
tensor<real32> right = place(tensor.ones<real32>([3]), args.device)
if args.case == 1
    right = place(tensor.ones<real32>([2, 2]), args.device)
if args.tracked
    left = left.track()
tensor<real32> output = left * right
print(output.shape()[0])
QUI
"$QUIDRA" build "$TMP/broadcast-errors.qui" -o "$TMP/broadcast-errors" >/dev/null
for case_index in 0 1; do
    expected="tensor broadcasting requires identical ranks"
    [[ "$case_index" -eq 1 ]] && expected="tensor shapes are not broadcast-compatible"
    for tracked in false true; do
        for run in cpu strided gather; do
            device=0
            mode="$run"
            if [[ "$run" == cpu ]]; then
                # The CPU ignores the switch; keep its value a valid one.
                device=-1
                mode=gather
            fi
            set +e
            QUIDRA_BROADCAST="$mode" "$TMP/broadcast-errors" --device "$device" \
                --case "$case_index" --tracked "$tracked" \
                >"$TMP/broadcast-error.out" 2>"$TMP/broadcast-error.err"
            status=$?
            set -e
            if [[ "$status" -ne 101 || -s "$TMP/broadcast-error.out" ]] ||
               ! grep -Fq "$expected" "$TMP/broadcast-error.err"; then
                echo "broadcast error case $case_index (tracked $tracked, $run) gave status $status:" >&2
                cat "$TMP/broadcast-error.out" "$TMP/broadcast-error.err" >&2
                exit 1
            fi
        done
    done
done

# Zero-extent broadcasting: a singleton axis takes the other side's
# extent, including 0, so [0, 3] with [1, 3] gives [0, 3] and the singleton
# operand's gradient is a sum over no elements. Equal ranks stay required;
# scalars broadcast to any rank. The CPU and the fake GPU in both broadcast
# modes print the same table, and the shapes that are not broadcast-
# compatible ([0,3] with [2,3]; [3] with [0,3], [2,3] or [1,3]; both orders)
# fail with a defined runtime error on each of them, with untracked and
# tracked operands.
cat > "$TMP/broadcast-zero-extent.expected" <<'TEXT'
zero-zero out [0,3]:
zero-zero d_left [0,3]:
zero-zero d_right [0,3]:
zero-one out [0,3]:
zero-one d_left [0,3]:
zero-one d_right [1,3]: 0.0 0.0 0.0
one-zero out [0,3]:
one-zero d_left [1,3]: 0.0 0.0 0.0
one-zero d_right [0,3]:
two-one out [2,3]: 11.0 42.0 93.0 44.0 105.0 186.0
two-one d_left [2,3]: 11.0 42.0 93.0 44.0 105.0 186.0
two-one d_right [1,3]: 17.0 29.0 45.0
zero-cols out [2,0]:
zero-cols d_left [2,0]:
zero-cols d_right [2,1]: 0.0 0.0
scalar out [2,3]: 3.0 4.0 5.0 6.0 7.0 8.0
scalar d_x [2,3]: 1.0 2.0 3.0 4.0 5.0 6.0
scalar-left out [2,3]: -1.0 -4.0 -7.0 -10.0 -13.0 -16.0
scalar-left d_x [2,3]: -3.0 -6.0 -9.0 -12.0 -15.0 -18.0
empty-scalar out [0,3]:
empty-scalar d_x [0,3]:
TEXT
"$QUIDRA" build "$SCRIPT_DIR/broadcast_zero_extent.qui" -o "$TMP/broadcast-zero-extent" >/dev/null
for run in cpu strided gather; do
    device=0
    mode="$run"
    if [[ "$run" == cpu ]]; then
        device=-1
        mode=gather
    fi
    QUIDRA_BROADCAST="$mode" "$TMP/broadcast-zero-extent" --device "$device" \
        >"$TMP/broadcast-zero-extent.out"
    if ! cmp -s "$TMP/broadcast-zero-extent.expected" "$TMP/broadcast-zero-extent.out"; then
        echo "zero-extent broadcasting ($run) differs from the expected table:" >&2
        diff "$TMP/broadcast-zero-extent.expected" "$TMP/broadcast-zero-extent.out" >&2 || true
        exit 1
    fi
    for case_index in 1 2 3 4 5 6 7 8; do
        expected="tensor broadcasting requires identical ranks"
        if [[ "$case_index" -eq 1 || "$case_index" -eq 5 ]]; then
            expected="tensor shapes are not broadcast-compatible"
        fi
        for tracked in 0 1 2 3; do
            set +e
            QUIDRA_BROADCAST="$mode" "$TMP/broadcast-zero-extent" --device "$device" \
                --case "$case_index" --tracked "$tracked" \
                >"$TMP/broadcast-zero-error.out" 2>"$TMP/broadcast-zero-error.err"
            status=$?
            set -e
            if [[ "$status" -ne 101 || -s "$TMP/broadcast-zero-error.out" ]] ||
               ! grep -Fq "$expected" "$TMP/broadcast-zero-error.err"; then
                echo "zero-extent broadcast error case $case_index tracked $tracked ($run) gave status $status:" >&2
                cat "$TMP/broadcast-zero-error.out" "$TMP/broadcast-zero-error.err" >&2
                exit 1
            fi
        done
    done
done

# Saved tensors of custom autograd nodes are forward-time snapshots, also
# when they share dense device storage copy-on-write
# (QUIDRA_SAVED_TENSORS=cow): writing the live tensor after the forward pass,
# through a package mutable handle or through a writable `&` reference,
# detaches it, and backward still reads the saved values.
mkdir -p "$TMP/packages/fake_saved/native"
cat > "$TMP/packages/fake_saved/quidra.package" <<'MANIFEST'
name = fake_saved
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST

cat > "$TMP/packages/fake_saved/main.qui" <<'QUI'
extern int32 scale_native(
    const tensor<real32> &input,
    const tensor<real32> &factor,
    tensor<real32> &output
) = "qsaved_scale"
extern int32 fill_native(
    tensor<real32> &value,
    real32 fill
) = "qsaved_fill"

// input * factor, with factor saved for backward.
tensor<real32> scale(tensor<real32> input, tensor<real32> factor)
    tensor<real32> output = tensor.zeros<real32>(input.shape(), gpu = nat(input.device()))
    int32 status = scale_native(&input, &factor, &output)
    if status != int32(0)
        error("fake-GPU saved-tensor registration failed")
    return output

int32 fill(tensor<real32> &value, real32 fill)
    return fill_native(&value, fill)
QUI

cat > "$TMP/packages/fake_saved/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

static const float* saved_read(const void* tensor) {
    const auto base = qcore_tensor_device_handle_const(tensor);
    if (base == 0) return nullptr;
    return reinterpret_cast<const float*>(
        base + qcore_tensor_device_offset_bytes(tensor));
}

static float* saved_write(void* tensor) {
    const auto base = qcore_tensor_device_handle(tensor);
    if (base == 0) return nullptr;
    return reinterpret_cast<float*>(
        base + qcore_tensor_device_offset_bytes(tensor));
}

static int qsaved_scale_backward(
    const void* const* saved_tensors, uint64_t saved_tensor_count,
    const void* gradient_output, void* const* gradient_inputs,
    uint64_t gradient_input_count, const void*, uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 || !gradient_output ||
        !gradient_inputs || gradient_input_count != 1)
        return 50;
    const auto count = qcore_tensor_element_count(gradient_output);
    const float* factor = saved_read(saved_tensors[0]);
    const float* gradient = saved_read(gradient_output);
    float* input_gradient = saved_write(gradient_inputs[0]);
    if (!factor || !gradient || !input_gradient) return 51;
    for (uint64_t index = 0; index < count; ++index)
        input_gradient[index] = gradient[index] * factor[index];
    return 0;
}

extern "C" int32_t qsaved_scale(
    const void* input, const void* factor, void* output) {
    if (qcore_tensor_backend(input) != QCORE_BACKEND_TEST) return 70;
    const auto count = qcore_tensor_element_count(input);
    const float* x = saved_read(input);
    const float* f = saved_read(factor);
    float* y = saved_write(output);
    if (!x || !f || !y) return 71;
    for (uint64_t index = 0; index < count; ++index) y[index] = x[index] * f[index];
    const void* inputs[] = {input};
    const void* saved[] = {factor};
    return qcore_tensor_attach_custom_autograd_with_saved(
        output, inputs, 1, saved, 1, qsaved_scale_backward, nullptr, 0);
}

extern "C" int32_t qsaved_fill(void* value, float fill) {
    float* data = saved_write(value);
    if (!data) return 1;
    const auto count = qcore_tensor_element_count(value);
    for (uint64_t index = 0; index < count; ++index) data[index] = fill;
    return 0;
}
CPP

cat > "$TMP/gpu-saved-writers.qui" <<'QUI'
import fake = fake_saved
void poke(tensor<real32> &value)
    value[1] = real32(-7)

tensor<real32> x = tensor.ones<real32>([4], gpu = 0)
tensor<real32> by_handle = tensor.ones<real32>([4], gpu = 0) * real32(3)
tensor<real32> by_reference = tensor.ones<real32>([4], gpu = 0) * real32(5)
tensor<real32> xt = x.track()
tensor<real32> y = fake.scale(xt, by_handle) + fake.scale(xt, by_reference)
print(fake.fill(&by_handle, real32(100)) == int32(0))
print(NL)
poke(&by_reference)
y.reshape([4]).scatter([0, 0, 0, 0], [1]).backward(&x)
tensor<real32> gradient = x.grad.cpu()
print("{real64(gradient[0].item())} {real64(gradient[1].item())} {real64(gradient[3].item())}")
print(NL)
tensor<real32> handle_live = by_handle.cpu()
tensor<real32> reference_live = by_reference.cpu()
print("{real64(handle_live[1].item())} {real64(reference_live[0].item())} {real64(reference_live[1].item())}")
print(NL)
QUI
for saved_mode in copy cow; do
    saved_output="$(QUIDRA_SAVED_TENSORS="$saved_mode" QUIDRA_PACKAGE_PATH="$TMP/packages" \
        "$QUIDRA" run "$TMP/gpu-saved-writers.qui" 2>&1)"
    if [[ "$saved_output" != "$(printf 'true\n8.0 8.0 8.0\n100.0 5.0 -7.0')" ]]; then
        echo "fake-GPU saved tensor changed after a write ($saved_mode saves):" >&2
        printf '%s\n' "$saved_output" >&2
        exit 1
    fi
done

# Package kernels borrow device storage through qcore_tensor_device_handle
# (_const). Like the CPU accessors, both refuse a view with an uninitialized
# element, so the package takes its portable path, which reports the read.
# Fully initialized storage and initialized views of partially initialized
# storage are accepted.
mkdir -p "$TMP/packages/fake_native/native"
cat > "$TMP/packages/fake_native/quidra.package" <<'MANIFEST'
name = fake_native
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST

cat > "$TMP/packages/fake_native/main.qui" <<'QUI'
extern int64 handle_const_native(
    const tensor<real32> &value
) = "qfake_handle_const"
extern int64 handle_mut_native(
    tensor<real32> &value
) = "qfake_handle_mut"
extern int64 handle_value_native(
    const tensor<real32> &value
) = "qfake_handle_value"
extern int32 scale2_native(
    const tensor<real32> &input,
    tensor<real32> &output
) = "qfake_scale2"
extern int32 mul_masked_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qfake_mul_masked"
extern int32 mul_masked_unreported_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qfake_mul_masked_unreported"
extern int32 mul_masked_undeclared_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output
) = "qfake_mul_masked_undeclared"
extern int64 masked_last_native() = "qfake_masked_last"
extern int64 fills_native() = "qfake_fills"

tensor<real32> mul_masked(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape(), gpu = nat(left.device()))
    int32 status = mul_masked_native(&left, &right, &output)
    if status != int32(0)
        error("fake-GPU masked custom autograd registration failed")
    return output

tensor<real32> mul_masked_unreported(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape(), gpu = nat(left.device()))
    int32 status = mul_masked_unreported_native(&left, &right, &output)
    if status != int32(0)
        error("fake-GPU masked custom autograd registration failed")
    return output

// The same callback without the full-write declaration.
tensor<real32> mul_masked_undeclared(tensor<real32> left, tensor<real32> right)
    tensor<real32> output = tensor.zeros<real32>(left.shape(), gpu = nat(left.device()))
    int32 status = mul_masked_undeclared_native(&left, &right, &output)
    if status != int32(0)
        error("fake-GPU masked custom autograd registration failed")
    return output

int masked_last()
    return int(masked_last_native())

// Device zero fills so far (QUIDRA_COUNTERS must be set).
int fills()
    return int(fills_native())

int handle_const(tensor<real32> value)
    return int(handle_const_native(&value))

int handle_mut(tensor<real32> value)
    return int(handle_mut_native(&value))

int handle_value(tensor<real32> value)
    return int(handle_value_native(&value))

tensor<real32> scale2(tensor<real32> input)
    tensor<real32> output = tensor.zeros<real32>(input.shape(), gpu = nat(input.device()))
    int32 status = scale2_native(&input, &output)
    if status != int32(0)
        error("fake-GPU custom autograd registration failed")
    return output
QUI

cat > "$TMP/packages/fake_native/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

static const float* fake_read(const void* tensor) {
    const auto base = qcore_tensor_device_handle_const(tensor);
    if (base == 0) return nullptr;
    return reinterpret_cast<const float*>(
        base + qcore_tensor_device_offset_bytes(tensor));
}

static float* fake_write(void* tensor) {
    const auto base = qcore_tensor_device_handle(tensor);
    if (base == 0) return nullptr;
    return reinterpret_cast<float*>(
        base + qcore_tensor_device_offset_bytes(tensor));
}

extern "C" long long qfake_handle_const(const void* value) {
    return fake_read(value) ? 1 : 0;
}

extern "C" long long qfake_handle_mut(void* value) {
    return fake_write(value) ? 1 : 0;
}

extern "C" long long qfake_handle_value(const void* value) {
    return static_cast<long long>(qcore_tensor_device_handle_const(value));
}

// The native ABI carries no strides, so a callback reads its upstream
// gradient as a dense run; Core densifies strided device gradients first.
static int qfake_scale2_backward(
    const void* const*, uint64_t, const void* gradient_output,
    void* const* gradient_inputs, uint64_t gradient_input_count,
    const void*, uint64_t) {
    if (!gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 60;
    if (!qcore_tensor_is_contiguous(gradient_output)) return 61;
    const auto count = qcore_tensor_element_count(gradient_output);
    const float* gradient = fake_read(gradient_output);
    float* input_gradient = fake_write(gradient_inputs[0]);
    if (!gradient || !input_gradient) return 62;
    for (uint64_t index = 0; index < count; ++index)
        input_gradient[index] = gradient[index] * 2.0F;
    return 0;
}

static long long qfake_masked_last_mask = -1;

// The metadata is the attach-time full_writes declaration: undeclared
// gradients must arrive zero-filled, declared ones are write-only outputs
// with unspecified contents.
static int qfake_mul_masked_backward(
    const void* const* saved_tensors, uint64_t saved_tensor_count,
    const void* gradient_output, void* const* gradient_inputs,
    const uint8_t* needed, uint8_t* fully_written,
    uint64_t gradient_input_count, const void* metadata, uint64_t metadata_size) {
    if (!saved_tensors || saved_tensor_count != 2 || !gradient_output ||
        !gradient_inputs || !needed || !fully_written ||
        gradient_input_count != 2 || !metadata || metadata_size != 2)
        return 80;
    const auto* declared = static_cast<const uint8_t*>(metadata);
    qfake_masked_last_mask = (needed[0] ? 1 : 0) + (needed[1] ? 2 : 0);
    const auto count = qcore_tensor_element_count(gradient_output);
    const float* x = fake_read(saved_tensors[0]);
    const float* y = fake_read(saved_tensors[1]);
    const float* g = fake_read(gradient_output);
    if (!x || !y || !g) return 81;
    for (uint64_t input = 0; input < 2; ++input) {
        if ((gradient_inputs[input] != nullptr) != (needed[input] != 0))
            return 82;
        if (!needed[input]) continue;
        float* out = fake_write(gradient_inputs[input]);
        if (!out) return 83;
        const float* other = input == 0 ? y : x;
        for (uint64_t index = 0; index < count; ++index) {
            if (!declared[input] && out[index] != 0.0F) return 84;
            out[index] = g[index] * other[index];
        }
        fully_written[input] = 1;
    }
    return 0;
}

static int32_t qfake_mul_attach(
    const void* left, const void* right, void* output,
    qcore_autograd_backward_masked_fn backward, const uint8_t* full_writes) {
    const auto count = qcore_tensor_element_count(left);
    const float* a = fake_read(left);
    const float* b = fake_read(right);
    float* out = fake_write(output);
    if (!a || !b || !out) return 90;
    for (uint64_t index = 0; index < count; ++index)
        out[index] = a[index] * b[index];
    const void* inputs[] = {left, right};
    const uint8_t none[] = {0, 0};
    const uint8_t* declared = full_writes ? full_writes : none;
    return qcore_tensor_attach_custom_autograd_masked(
        output, inputs, 2, inputs, 2, backward, nullptr, full_writes,
        declared, 2);
}

extern "C" int32_t qfake_mul_masked(
    const void* left, const void* right, void* output) {
    const uint8_t full_writes[] = {1, 1};
    return qfake_mul_attach(left, right, output, qfake_mul_masked_backward, full_writes);
}

extern "C" int32_t qfake_mul_masked_undeclared(
    const void* left, const void* right, void* output) {
    return qfake_mul_attach(left, right, output, qfake_mul_masked_backward, nullptr);
}

extern "C" long long qfake_fills() {
    return static_cast<long long>(qcore_counter_value("fills"));
}

// Declares full writes but reports none for input 0.
static int qfake_mul_masked_unreported_backward(
    const void* const* saved_tensors, uint64_t saved_tensor_count,
    const void* gradient_output, void* const* gradient_inputs,
    const uint8_t* needed, uint8_t* fully_written,
    uint64_t gradient_input_count, const void* metadata, uint64_t metadata_size) {
    const int status = qfake_mul_masked_backward(
        saved_tensors, saved_tensor_count, gradient_output, gradient_inputs,
        needed, fully_written, gradient_input_count, metadata, metadata_size);
    fully_written[0] = 0;
    return status;
}

extern "C" int32_t qfake_mul_masked_unreported(
    const void* left, const void* right, void* output) {
    const uint8_t full_writes[] = {1, 1};
    return qfake_mul_attach(
        left, right, output, qfake_mul_masked_unreported_backward, full_writes);
}

extern "C" long long qfake_masked_last() { return qfake_masked_last_mask; }

extern "C" int32_t qfake_scale2(const void* input, void* output) {
    if (qcore_tensor_backend(input) != QCORE_BACKEND_TEST ||
        qcore_tensor_backend(output) != QCORE_BACKEND_TEST)
        return 70;
    const auto count = qcore_tensor_element_count(input);
    const float* source = fake_read(input);
    float* destination = fake_write(output);
    if (!source || !destination) return 71;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * 2.0F;
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd(
        output, inputs, 1, qfake_scale2_backward, nullptr, 0);
}
CPP

cat > "$TMP/gpu-native-handles.qui" <<'QUI'
import fake = fake_native
tensor<real32> full = tensor.ones<real32>([4], gpu = 0)
print(fake.handle_const(full) == 1 and fake.handle_mut(full) == 1)
print(NL)
tensor<real32> partial = tensor<real32>([4], gpu = 0)
partial[0] = real32(1)
partial[1] = real32(2)
print(fake.handle_const(partial) == 0 and fake.handle_mut(partial) == 0)
print(NL)
print(fake.handle_const(partial[0:2]) == 1 and fake.handle_mut(partial[0:2]) == 1)
print(NL)
print(fake.handle_const(partial[1:3]) == 0 and fake.handle_mut(partial[1:3]) == 0)
print(NL)
partial[2] = real32(3)
partial[3] = real32(4)
print(fake.handle_const(partial) == 1 and fake.handle_mut(partial) == 1)
print(NL)
QUI
native_handle_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/gpu-native-handles.qui")"
if [[ "$native_handle_output" != "$(for _ in {1..5}; do echo true; done)" ]]; then
    echo "unexpected fake-GPU native handle initialization result:" >&2
    printf '%s\n' "$native_handle_output" >&2
    exit 1
fi

# A broadcast operand that is a view into a larger storage enters the graph
# as a dense operand-sized copy instead of keeping the whole storage alive:
# writing the larger tensor while the graph lives then needs no
# copy-on-write, so its device buffer stays the same. Backward still reads
# the forward-time values. Both broadcast modes behave alike.
cat > "$TMP/gpu-broadcast-retention.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
import fake = fake_native
tensor<real32> big = tensor.ones<real32>([64, 2], gpu = 0) * real32(3)
tensor<real32> x = tensor.ones<real32>([5, 2], gpu = 0)
tensor<real32> product = x.track() * big[1:2]
int before = fake.handle_value(big)
big[1, 0] = real32(-7)
print(before != 0 and fake.handle_value(big) == before)
print(NL)
reductions.sum(product).backward(&x)
tensor<real32> gradient = x.grad.cpu()
print(gradient[4, 0].item() == real32(3) and gradient[0, 1].item() == real32(3))
print(NL)
QUI
for variant in strided gather; do
    retention_output="$(QUIDRA_BROADCAST="$variant" QUIDRA_PACKAGE_PATH="$TMP/packages" \
        "$QUIDRA" run "$TMP/gpu-broadcast-retention.qui" 2>&1)"
    if [[ "$retention_output" != "$(printf 'true\ntrue')" ]]; then
        echo "unexpected fake-GPU broadcast operand retention ($variant):" >&2
        printf '%s\n' "$retention_output" >&2
        exit 1
    fi
done

# Transpose backward hands a strided device gradient upstream. A custom
# native callback, which sees no strides, must receive it as a dense copy.
cat > "$TMP/gpu-native-strided-gradient.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
import fake = fake_native
tensor<real32> source = tensor.ones<real32>([2, 3], gpu = 0)
tensor<real32> weights = tensor<real32>([3, 2], gpu = 0)
for row in range(3)
    for column in range(2)
        weights[row, column] = real32(row * 2 + column + 1)
tensor<real32> x = source.track()
tensor<real32> scaled = fake.scale2(x)
tensor<real32> loss = reductions.sum(scaled.transpose(0, 1) * weights)
loss.backward(&source)
tensor<real32> gradient = source.grad.cpu()
bool matches = true
for row in range(2)
    for column in range(3)
        real32 weight = real32(column * 2 + row + 1)
        real32 expected = real32(2) * weight
        if gradient[row, column].item() != expected
            matches = false
print(matches)
print(NL)
QUI
set +e
strided_gradient_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/gpu-native-strided-gradient.qui" 2>&1)"
strided_gradient_status=$?
set -e
if [[ "$strided_gradient_status" -ne 0 || "$strided_gradient_output" != "true" ]]; then
    echo "unexpected fake-GPU custom backward result for a strided gradient (status $strided_gradient_status):" >&2
    printf '%s\n' "$strided_gradient_output" >&2
    exit 1
fi

# The device engine passes the gradient-need mask to masked callbacks.
cat > "$TMP/gpu-native-masked.qui" <<'QUI'
import fake = fake_native
tensor<real32> x = tensor.ones<real32>([2], gpu = 0) * real32(3)
tensor<real32> y = tensor.ones<real32>([2], gpu = 0) * real32(5)
tensor<real32> product = fake.mul_masked(x.track(), y.track())
tensor<real32> loss = product.gather([0], []) + product.gather([1], [])
loss.backward(&y)
print(y.grad.cpu()[1].item() == real32(3) and fake.masked_last() == 2)
print(NL)
loss.backward(&x, &y)
print(x.grad.cpu()[0].item() == real32(5) and y.grad.cpu()[0].item() == real32(6) and fake.masked_last() == 3)
print(NL)
QUI
masked_gpu_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/gpu-native-masked.qui" 2>&1)"
if [[ "$masked_gpu_output" != "$(printf 'true\ntrue')" ]]; then
    echo "unexpected fake-GPU masked custom autograd result:" >&2
    printf '%s\n' "$masked_gpu_output" >&2
    exit 1
fi

# Gradients a masked callback declares fully written (full_writes) are
# write-only outputs: the fake GPU skips their zero fill (one fill per
# gradient fewer than the undeclared callback pays), and the gradients are
# bitwise those of the undeclared callback. QUIDRA_GPU_ZERO_FILL=always
# restores the fill.
cat > "$TMP/gpu-native-masked-fill.qui" <<'QUI'
import fake = fake_native
tensor<real32> xs = tensor.zeros<real32>([3])
xs[0] = real32(0.1)
xs[1] = real32(-2.5)
xs[2] = real32(3.7)
tensor<real32> ys = tensor.zeros<real32>([3])
ys[0] = real32(1.3)
ys[1] = real32(0.7)
ys[2] = real32(-0.9)
tensor<real32> weights = tensor.zeros<real32>([3])
weights[0] = real32(0.25)
weights[1] = real32(-1.5)
weights[2] = real32(2.125)

tensor<real32> x = xs.gpu(0)
tensor<real32> y = ys.gpu(0)
int start = fake.fills()
tensor<real32> declared = fake.mul_masked(x.track(), y.track()) * weights.gpu(0)
(declared.gather([0], []) + declared.gather([1], []) + declared.gather([2], [])).backward(&x, &y)
int declared_fills = fake.fills() - start

tensor<real32> u = xs.gpu(0)
tensor<real32> v = ys.gpu(0)
start = fake.fills()
tensor<real32> undeclared = fake.mul_masked_undeclared(u.track(), v.track()) * weights.gpu(0)
(undeclared.gather([0], []) + undeclared.gather([1], []) + undeclared.gather([2], [])).backward(&u, &v)
int undeclared_fills = fake.fills() - start

print((x.grad.cpu() == u.grad.cpu()).all() and (y.grad.cpu() == v.grad.cpu()).all())
print(NL)
print((x.grad.cpu() == ys * weights).all() and (y.grad.cpu() == xs * weights).all())
print(NL)
print(undeclared_fills - declared_fills)
print(NL)
QUI
masked_fill_output="$(QUIDRA_COUNTERS="$TMP/masked-fill.jsonl" QUIDRA_PACKAGE_PATH="$TMP/packages" \
    "$QUIDRA" run "$TMP/gpu-native-masked-fill.qui" 2>&1)"
if [[ "$masked_fill_output" != "$(printf 'true\ntrue\n2')" ]]; then
    echo "fake-GPU declared full writes are not write-only outputs:" >&2
    printf '%s\n' "$masked_fill_output" >&2
    exit 1
fi
masked_fill_output="$(QUIDRA_GPU_ZERO_FILL=always QUIDRA_COUNTERS="$TMP/masked-fill-always.jsonl" \
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/gpu-native-masked-fill.qui" 2>&1)"
if [[ "$masked_fill_output" != "$(printf 'true\ntrue\n0')" ]]; then
    echo "fake-GPU declared full writes with QUIDRA_GPU_ZERO_FILL=always:" >&2
    printf '%s\n' "$masked_fill_output" >&2
    exit 1
fi

# On the device engine too, a declared full write must be reported. Only the
# undeclared-report input matters: y's gradient alone passes, x's fails.
cat > "$TMP/gpu-native-masked-unreported.qui" <<'QUI'
import fake = fake_native
tensor<real32> x = tensor.ones<real32>([2], gpu = 0) * real32(3)
tensor<real32> y = tensor.ones<real32>([2], gpu = 0) * real32(5)
tensor<real32> product = fake.mul_masked_unreported(x.track(), y.track())
tensor<real32> loss = product.gather([0], []) + product.gather([1], [])
loss.backward(&y)
print(y.grad.cpu()[1].item() == real32(3))
print(NL)
loss.backward(&x)
print(x.grad.cpu()[1].item())
print(NL)
QUI
set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/gpu-native-masked-unreported.qui" \
    >"$TMP/gpu-native-masked-unreported.out" 2>"$TMP/gpu-native-masked-unreported.err"
unreported_status=$?
set -e
if [[ "$unreported_status" -eq 0 ]] ||
   [[ "$(cat "$TMP/gpu-native-masked-unreported.out")" != "true" ]] ||
   ! grep -q "did not report a complete write of gradient input 0 declared as fully written" \
        "$TMP/gpu-native-masked-unreported.err"; then
    echo "fake-GPU unreported declared full write was accepted (status $unreported_status):" >&2
    cat "$TMP/gpu-native-masked-unreported.out" "$TMP/gpu-native-masked-unreported.err" >&2
    exit 1
fi

echo "fake GPU autograd and broadcast contracts: ok"
