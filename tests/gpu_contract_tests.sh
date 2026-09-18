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

cat > "$TMP/gpu-compute.qui" <<'QUI'
tensor<float32> left = tensor.ones<float32>([2], gpu = 0)
tensor<float32> right = tensor.ones<float32>([2], gpu = 0)
tensor<float32> added = left + right
tensor<float32> scaled = added * 2.0
tensor<float32> reversed = 10.0 - scaled
tensor<float32> divided = reversed / 2.0
tensor<float32> negated = -left

print(added[0].item())
print(scaled[1].item())
print(divided[0].item())
print(negated[0].item())

tensor<int> values = tensor.zeros<int>([3], gpu = 0)
values[0] = 1
values[1] = 2
values[2] = 3
tensor<float> converted = float(values)
print(converted[2].item())
print(stats.sum(values))
print(stats.min(values))
print(stats.max(values))
print(stats.mean(values))

tensor<float32> vector_a = tensor.ones<float32>([2], gpu = 0)
tensor<float32> vector_b = tensor.ones<float32>([2], gpu = 0)
print(linear.dot(vector_a, vector_b))

tensor<float32> matrix_a = tensor.ones<float32>([2, 3], gpu = 0)
tensor<float32> matrix_b = tensor.ones<float32>([3, 2], gpu = 0)
tensor<float32> product = linear.matmul(matrix_a, matrix_b)
print(product[0, 0].item())
print(product[1, 1].item())

tensor<float32> cpu_reference = product.cpu()
print(cpu_reference[0, 0].item())
QUI

gpu_compute_output="$("$QUIDRA" run "$TMP/gpu-compute.qui")"
gpu_compute_expected="$(printf '2.0\n4.0\n3.0\n-1.0\n3.0\n6\n1\n3\n2.0\n2.0\n3.0\n3.0\n3.0')"
if [[ "$gpu_compute_output" != "$gpu_compute_expected" ]]; then
    echo "unexpected fake-GPU compute output:" >&2
    printf '%s\n' "$gpu_compute_output" >&2
    exit 1
fi

cat > "$TMP/gpu-view.qui" <<'QUI'
tensor<int> value = tensor.zeros<int>([2, 3], gpu = 0)
value[0, 0] = 1
value[0, 1] = 2
value[0, 2] = 3
value[1, 0] = 4
value[1, 1] = 5
value[1, 2] = 6
tensor<int> view = value[0:2, 1:3]
tensor<int> dense = view.contiguous()
print(dense.shape()[0])
print(dense.shape()[1])
print(dense[0, 0].item())
print(dense[1, 1].item())
QUI
gpu_view_output="$("$QUIDRA" run "$TMP/gpu-view.qui")"
gpu_view_expected="$(printf '2\n2\n2\n6')"
if [[ "$gpu_view_output" != "$gpu_view_expected" ]]; then
    echo "unexpected fake-GPU view output:" >&2
    printf '%s\n' "$gpu_view_output" >&2
    exit 1
fi

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
