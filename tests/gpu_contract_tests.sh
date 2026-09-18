#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# This environment variable is recognized only when Quidra was compiled with
# QUIDRA_ENABLE_TEST_GPU_BACKEND. Production builds contain no fake backend.
export QUIDRA_TEST_FAKE_GPU_COUNT=2

gpu_info="$("$QUIDRA" gpu)"
grep -Fq "GPU 0" <<<"$gpu_info"
grep -Fq "GPU 1" <<<"$gpu_info"
grep -Fq "backend: TEST" <<<"$gpu_info"
grep -Fq "status: supported" <<<"$gpu_info"

cat > "$TMP/transfers.qui" <<'QUI'
tensor<int> cpu = tensor.zeros<int>([3])
cpu[0] = 10
cpu[1] = 20
cpu[2] = 30

tensor<int> gpu0 = cpu.gpu(0)
tensor<int> same_gpu_copy = gpu0.gpu(0)
tensor<int> gpu1 = same_gpu_copy.gpu(1)
tensor<int> roundtrip = gpu1.cpu()

print(roundtrip[0].item())
print(roundtrip[1].item())
print(roundtrip[2].item())

tensor<int> direct_ones = tensor.ones<int>([2], gpu = 1)
tensor<int> ones_cpu = direct_ones.cpu()
print(ones_cpu[0].item())
print(ones_cpu[1].item())

tensor<int> direct_zeros = tensor.zeros<int>([2], gpu = 0)
tensor<int> zeros_cpu = direct_zeros.cpu()
print(zeros_cpu[0].item())
print(zeros_cpu[1].item())

tensor<int> reshaped_gpu = direct_ones.reshape([1, 2])
tensor<int> reshaped_cpu = reshaped_gpu.cpu()
print(reshaped_cpu.shape()[0])
print(reshaped_cpu.shape()[1])

tensor<int> contiguous_gpu = direct_ones.contiguous()
tensor<int> contiguous_cpu = contiguous_gpu.cpu()
print(contiguous_cpu[0].item())
QUI

transfer_output="$("$QUIDRA" run "$TMP/transfers.qui")"
transfer_expected="$(printf '10\n20\n30\n1\n1\n0\n0\n1\n2\n1')"
if [[ "$transfer_output" != "$transfer_expected" ]]; then
    echo "unexpected fake-GPU transfer output:" >&2
    printf '%s\n' "$transfer_output" >&2
    exit 1
fi

expect_runtime_error() {
    local file="$1"
    local expected="$2"
    set +e
    "$QUIDRA" run "$file" >"$file.out" 2>"$file.err"
    local status=$?
    set -e
    if [[ $status -ne 101 ]]; then
        echo "expected runtime status 101 for $file, got $status" >&2
        cat "$file.out" >&2 || true
        cat "$file.err" >&2 || true
        exit 1
    fi
    if ! grep -Fq "$expected" "$file.err"; then
        echo "missing diagnostic '$expected' for $file" >&2
        cat "$file.err" >&2
        exit 1
    fi
    if [[ -s "$file.out" ]]; then
        echo "operation produced output before failing: $file" >&2
        cat "$file.out" >&2
        exit 1
    fi
}

cat > "$TMP/cpu-gpu-mismatch.qui" <<'QUI'
tensor<float32> cpu = tensor.ones<float32>([2])
tensor<float32> gpu = tensor.ones<float32>([2], gpu = 0)
tensor<float32> invalid = cpu + gpu
print(invalid.shape()[0])
QUI
expect_runtime_error "$TMP/cpu-gpu-mismatch.qui" "tensor operands are on different devices"

cat > "$TMP/gpu-gpu-mismatch.qui" <<'QUI'
tensor<float32> gpu0 = tensor.ones<float32>([2], gpu = 0)
tensor<float32> gpu1 = tensor.ones<float32>([2], gpu = 1)
tensor<float32> invalid = gpu0 + gpu1
print(invalid.shape()[0])
QUI
expect_runtime_error "$TMP/gpu-gpu-mismatch.qui" "tensor operands are on different devices"

cat > "$TMP/same-gpu-no-fallback.qui" <<'QUI'
tensor<float32> left = tensor.ones<float32>([2], gpu = 0)
tensor<float32> right = tensor.ones<float32>([2], gpu = 0)
tensor<float32> invalid = left + right
print(invalid.shape()[0])
QUI
expect_runtime_error "$TMP/same-gpu-no-fallback.qui" "tensor arithmetic is not supported on gpu(0)"

cat > "$TMP/scalar-kernel-argument.qui" <<'QUI'
tensor<float32> value = tensor.ones<float32>([2], gpu = 0)
tensor<float32> invalid = value + 1.0
print(invalid.shape()[0])
QUI
expect_runtime_error "$TMP/scalar-kernel-argument.qui" "tensor arithmetic is not supported on gpu(0)"
if grep -Fq "different devices" "$TMP/scalar-kernel-argument.qui.err"; then
    echo "scalar operand was incorrectly treated as a tensor device mismatch" >&2
    exit 1
fi

cat > "$TMP/item-no-hidden-download.qui" <<'QUI'
tensor<float32> value = tensor.ones<float32>([1], gpu = 0)
print(value[0].item())
QUI
expect_runtime_error "$TMP/item-no-hidden-download.qui" "tensor.item is not supported on gpu(0)"

cat > "$TMP/cast-no-fallback.qui" <<'QUI'
tensor<int> value = tensor.ones<int>([1], gpu = 0)
tensor<float> converted = float(value)
print(converted.shape()[0])
QUI
expect_runtime_error "$TMP/cast-no-fallback.qui" "tensor.cast is not supported on gpu(0)"

cat > "$TMP/dot-no-fallback.qui" <<'QUI'
tensor<float32> left = tensor.ones<float32>([2], gpu = 0)
tensor<float32> right = tensor.ones<float32>([2], gpu = 0)
float32 result = linear.dot(left, right)
print(result)
QUI
expect_runtime_error "$TMP/dot-no-fallback.qui" "linear.dot is not supported on gpu(0)"

cat > "$TMP/mean-no-fallback.qui" <<'QUI'
tensor<float32> value = tensor.ones<float32>([2], gpu = 0)
float result = stats.mean(value)
print(result)
QUI
expect_runtime_error "$TMP/mean-no-fallback.qui" "stats.mean is not supported on gpu(0)"

cat > "$TMP/image-write-no-fallback.qui" <<'QUI'
tensor<uint8> value = tensor.ones<uint8>([1, 1, 1], gpu = 0)
auto written = image.write("should-not-exist.png", value)
match written
    void
        print("unexpected success")
    error problem
        print(problem)
QUI
image_write_output="$(cd "$TMP" && "$QUIDRA" run "$TMP/image-write-no-fallback.qui")"
if [[ "$image_write_output" != "image.write is not supported on gpu(0)" ]]; then
    echo "unexpected GPU image.write diagnostic: $image_write_output" >&2
    exit 1
fi
if [[ -e "$TMP/should-not-exist.png" ]]; then
    echo "GPU image.write unexpectedly produced a CPU-fallback file" >&2
    exit 1
fi

echo "fake GPU placement contracts: ok"
