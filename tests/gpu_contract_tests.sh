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

tensor<float32> row_vector = tensor.ones<float32>([3], gpu = 0)
tensor<float32> vector_matrix = linear.matmul(row_vector, matrix_b)
print(vector_matrix.shape()[0])
print(vector_matrix[1].item())

tensor<float32> column_vector = tensor.ones<float32>([3], gpu = 0)
tensor<float32> matrix_vector = linear.matmul(matrix_a, column_vector)
print(matrix_vector.shape()[0])
print(matrix_vector[1].item())

tensor<float32> cpu_matrix = tensor.ones<float32>([2, 3])
tensor<float32> cpu_vector = tensor.ones<float32>([3])
tensor<float32> cpu_matrix_vector = linear.matmul(cpu_matrix, cpu_vector)
print(cpu_matrix_vector[1].item())

tensor<float32> cpu_reference = product.cpu()
print(cpu_reference[0, 0].item())

tensor<float32> scalar_base = tensor.ones<float32>([1], gpu = 0) * 4.0
print((scalar_base + 2.0)[0].item())
print((2.0 + scalar_base)[0].item())
print((scalar_base - 2.0)[0].item())
print((10.0 - scalar_base)[0].item())
print((scalar_base * 2.0)[0].item())
print((2.0 * scalar_base)[0].item())
print((scalar_base / 2.0)[0].item())
print((8.0 / scalar_base)[0].item())
QUI

gpu_compute_output="$("$QUIDRA" run "$TMP/gpu-compute.qui")"
gpu_compute_expected="$(printf '2.0\n4.0\n3.0\n-1.0\n3.0\n6\n1\n3\n2.0\n2.0\n3.0\n3.0\n2\n3.0\n2\n3.0\n3.0\n3.0\n6.0\n6.0\n2.0\n6.0\n8.0\n8.0\n2.0\n2.0')"
if [[ "$gpu_compute_output" != "$gpu_compute_expected" ]]; then
    echo "unexpected fake-GPU compute output:" >&2
    printf '%s\n' "$gpu_compute_output" >&2
    exit 1
fi

cat > "$TMP/gpu-neural-unary.qui" <<'QUI'
tensor<float> value = tensor.ones<float>([1], gpu = 0)
neural<float> exponential = neural.exponential(neural.track(value))
neural<float> restored = neural.logarithm(exponential)
float result = restored.untrack().item()
print(result > 0.999999999 and result < 1.000000001)
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-neural-unary.qui")" != "true" ]]; then
    echo "unexpected fake-GPU float64 neural exp/log result" >&2
    exit 1
fi

cat > "$TMP/gpu-log-domain.qui" <<'QUI'
tensor<float32> value = tensor.zeros<float32>([1], gpu = 0)
neural<float32> invalid = neural.logarithm(neural.track(value))
print(invalid.untrack().item())
QUI
expect_runtime_error "$TMP/gpu-log-domain.qui" "logarithm requires finite positive values"

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

tensor<int> direct_cpu = view.cpu()
print(direct_cpu[0, 0].item())
print(direct_cpu[1, 1].item())

tensor<int> cross_gpu = view.gpu(1)
tensor<int> cross_cpu = cross_gpu.cpu()
print(cross_cpu[0, 0].item())
print(cross_cpu[1, 1].item())

tensor<int><3, 2> transposed = value.transpose(0, 1)
print(transposed.shape()[0])
print(transposed.shape()[1])
print(transposed[2, 1].item())
print(transposed.is_contiguous())
tensor<int> transpose_dense = transposed.contiguous()
print(transpose_dense[2, 1].item())
QUI
gpu_view_output="$("$QUIDRA" run "$TMP/gpu-view.qui")"
gpu_view_expected="$(printf '2\n2\n2\n6\n2\n6\n2\n6\n3\n2\n6\nfalse\n6')"
if [[ "$gpu_view_output" != "$gpu_view_expected" ]]; then
    echo "unexpected fake-GPU view output:" >&2
    printf '%s\n' "$gpu_view_output" >&2
    exit 1
fi


cat > "$TMP/integer-dtypes.qui" <<'QUI'
tensor<int8> i8 = tensor.ones<int8>([2], gpu = 0) + int8(2)
tensor<int16> i16 = tensor.ones<int16>([2], gpu = 0) * int16(3)
tensor<int32> i32 = tensor.ones<int32>([2], gpu = 0) - int32(4)
tensor<int16> neg_i16 = -tensor.ones<int16>([2], gpu = 0)
tensor<int> i64 = tensor.ones<int>([2], gpu = 0) + 5
tensor<uint8> u8 = tensor.ones<uint8>([2], gpu = 0) + uint8(6)
tensor<uint16> u16 = tensor.ones<uint16>([2], gpu = 0) * uint16(7)
tensor<uint32> u32 = tensor.ones<uint32>([2], gpu = 0) + uint32(8)
tensor<uint64> u64 = tensor.ones<uint64>([2], gpu = 0) + uint64(9)

print(i8.cpu()[0].item())
print(i16.cpu()[0].item())
print(i32.cpu()[0].item())
print(neg_i16.cpu()[0].item())
print(i64.cpu()[0].item())
print(u8.cpu()[0].item())
print(u16.cpu()[0].item())
print(u32.cpu()[0].item())
print(u64.cpu()[0].item())

tensor<int8> cast_source = tensor.ones<int8>([2], gpu = 0)
tensor<uint16> casted = uint16(cast_source)
print(casted.cpu()[1].item())

tensor<int16> dot_a = tensor.ones<int16>([3], gpu = 0)
tensor<int16> dot_b = tensor.ones<int16>([3], gpu = 0)
print(linear.dot(dot_a, dot_b))

tensor<int32> matrix_a = tensor.ones<int32>([2, 2], gpu = 0)
tensor<int32> matrix_b = tensor.ones<int32>([2, 2], gpu = 0)
tensor<int32> matrix_c = linear.matmul(matrix_a, matrix_b)
print(matrix_c.cpu()[1, 1].item())

print(stats.sum(u32))
print(stats.min(i32))
print(stats.max(u64))
print(stats.mean(i16))
QUI

integer_output="$("$QUIDRA" run "$TMP/integer-dtypes.qui")"
integer_expected="$(printf '3\n3\n-3\n-1\n6\n7\n7\n9\n10\n1\n3\n2\n18\n-3\n10\n3.0')"
if [[ "$integer_output" != "$integer_expected" ]]; then
    echo "unexpected fake-GPU integer dtype output:" >&2
    printf '%s\n' "$integer_output" >&2
    exit 1
fi

cat > "$TMP/integer-overflow.qui" <<'QUI'
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(127)
tensor<int8> invalid = value + int8(1)
print(invalid.cpu()[0].item())
QUI
expect_runtime_error "$TMP/integer-overflow.qui" "tensor integer arithmetic overflow"

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
