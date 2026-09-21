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

cat > "$TMP/explicit-sync.qui" <<'QUI'
gpu.sync(0)
gpu.sync(index = 1)
print("sync-ok")
QUI
if [[ "$("$QUIDRA" run "$TMP/explicit-sync.qui")" != "sync-ok" ]]; then
    echo "explicit gpu.sync contract failed" >&2
    exit 1
fi

cat > "$TMP/time-sync-mode.qui" <<'QUI'
time.Instant default_start = time.now()
time.Duration default_elapsed = time.since(default_start)
print(default_elapsed.seconds() >= 0.0)

time.Instant async_start = time.now(sync = false)
time.Duration async_elapsed = time.since(async_start, sync = false)
print(async_elapsed.seconds() >= 0.0)

time.Instant synced_start = time.now(sync = true)
time.Duration synced_elapsed = time.since(synced_start, sync = true)
print(synced_elapsed.seconds() >= 0.0)
QUI
time_sync_output="$("$QUIDRA" run "$TMP/time-sync-mode.qui")"
if [[ "$time_sync_output" != "$(printf 'true\ntrue\ntrue')" ]]; then
    echo "unexpected time synchronization-mode output:" >&2
    printf '%s\n' "$time_sync_output" >&2
    exit 1
fi

cat > "$TMP/invalid-sync-device.qui" <<'QUI'
gpu.sync(2)
QUI
set +e
"$QUIDRA" run "$TMP/invalid-sync-device.qui" >"$TMP/invalid-sync-device.out" 2>"$TMP/invalid-sync-device.err"
invalid_sync_status=$?
set -e
if [[ $invalid_sync_status -ne 101 ]]; then
    echo "gpu.sync unavailable-device status mismatch: $invalid_sync_status" >&2
    exit 1
fi
grep -Fq "gpu(2) is not available" "$TMP/invalid-sync-device.err"

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

cat > "$TMP/gpu-copy-on-write.qui" <<'QUI'
tensor<int> original = tensor.ones<int>([2], gpu = 0)
tensor<int> copied = original
print(&original != &copied)
copied[0] = 9
print(original.cpu()[0].item())
print(copied.cpu()[0].item())
QUI

gpu_cow_output="$("$QUIDRA" run "$TMP/gpu-copy-on-write.qui")"
gpu_cow_expected="$(printf 'true\n1\n9')"
if [[ "$gpu_cow_output" != "$gpu_cow_expected" ]]; then
    echo "unexpected fake-GPU copy-on-write output:" >&2
    printf '%s\n' "$gpu_cow_output" >&2
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


cat > "$TMP/time-async-does-not-sync.qui" <<'QUI'
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Instant default_start = time.now()
time.Duration default_elapsed = time.since(default_start)
time.Instant explicit_start = time.now(sync = false)
time.Duration explicit_elapsed = time.since(explicit_start, sync = false)
print(default_elapsed.seconds() >= 0.0 and explicit_elapsed.seconds() >= 0.0)
QUI
time_async_output="$(QUIDRA_TEST_FAKE_GPU_SYNC_FAIL=1 "$QUIDRA" run "$TMP/time-async-does-not-sync.qui")"
if [[ "$time_async_output" != "true" ]]; then
    echo "time sync=false unexpectedly synchronized fake GPU" >&2
    printf '%s\n' "$time_async_output" >&2
    exit 1
fi

cat > "$TMP/time-now-syncs-when-requested.qui" <<'QUI'
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Instant synchronized = time.now(sync = true)
print("unreachable")
QUI
set +e
QUIDRA_TEST_FAKE_GPU_SYNC_FAIL=1 "$QUIDRA" run "$TMP/time-now-syncs-when-requested.qui" >"$TMP/time-now-syncs-when-requested.out" 2>"$TMP/time-now-syncs-when-requested.err"
time_now_sync_status=$?
set -e
if [[ $time_now_sync_status -ne 101 ]]; then
    echo "time.now(sync = true) did not enter the GPU synchronization boundary" >&2
    exit 1
fi
grep -Fq "test-only fake GPU synchronization failure" "$TMP/time-now-syncs-when-requested.err"

cat > "$TMP/time-since-syncs-when-requested.qui" <<'QUI'
time.Instant start = time.now(sync = false)
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Duration synchronized = time.since(start, sync = true)
print(synchronized.seconds())
QUI
set +e
QUIDRA_TEST_FAKE_GPU_SYNC_FAIL=1 "$QUIDRA" run "$TMP/time-since-syncs-when-requested.qui" >"$TMP/time-since-syncs-when-requested.out" 2>"$TMP/time-since-syncs-when-requested.err"
time_since_sync_status=$?
set -e
if [[ $time_since_sync_status -ne 101 ]]; then
    echo "time.since(start, sync = true) did not enter the GPU synchronization boundary" >&2
    exit 1
fi
grep -Fq "test-only fake GPU synchronization failure" "$TMP/time-since-syncs-when-requested.err"

cat > "$TMP/time-sync-used-devices-only.qui" <<'QUI'
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Instant start = time.now(sync = true)
time.Duration elapsed = time.since(start, sync = true)
print(elapsed.seconds() >= 0.0)
QUI
time_used_devices_output="$(QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX=1 "$QUIDRA" run "$TMP/time-sync-used-devices-only.qui")"
if [[ "$time_used_devices_output" != "true" ]]; then
    echo "synchronized timing touched an unused fake GPU" >&2
    printf '%s\n' "$time_used_devices_output" >&2
    exit 1
fi

cat > "$TMP/fake-sync-consumes-validation.qui" <<'QUI'
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(127)
tensor<int8> invalid = value + int8(1)
gpu.sync(0)
print("unreachable")
QUI
expect_runtime_error "$TMP/fake-sync-consumes-validation.qui" "tensor integer arithmetic overflow"
if [[ -s "$TMP/fake-sync-consumes-validation.qui.out" ]]; then
    echo "fake GPU synchronization did not stop at deferred validation failure" >&2
    cat "$TMP/fake-sync-consumes-validation.qui.out" >&2
    exit 1
fi

cat > "$TMP/cpu-gpu-mismatch.qui" <<'QUI'
tensor<float32> cpu = tensor.ones<float32>([2])
tensor<float32> gpu_value = tensor.ones<float32>([2], gpu = 0)
tensor<float32> invalid = cpu + gpu_value
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

tensor<float32> compare_high = tensor.ones<float32>([2], gpu = 0) * 2.0
print(left == right)
print(left != compare_high)
print(left < compare_high)
print(left <= compare_high)
print(left > compare_high)
print(compare_high >= left)
tensor<float32> compare_mixed = tensor.ones<float32>([2], gpu = 0)
compare_mixed[1] = 2.0
print(left != compare_mixed)

tensor<float32> compare_matrix = tensor.ones<float32>([2, 3], gpu = 0)
tensor<float32> compare_view_a = compare_matrix[0:2, 1:3]
tensor<float32> compare_view_b = compare_matrix[0:2, 1:3]
print(compare_view_a == compare_view_b)

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

tensor<int> direct = tensor<int>([2], gpu = 0)
direct[0] = 4
direct[1] = 5
print(direct[1].item())
QUI

gpu_compute_output="$("$QUIDRA" run "$TMP/gpu-compute.qui")"
gpu_compute_expected="$(printf '2.0\n4.0\n3.0\n-1.0\ntrue\ntrue\ntrue\ntrue\nfalse\ntrue\nfalse\ntrue\n3.0\n6\n1\n3\n2.0\n2.0\n3.0\n3.0\n2\n3.0\n2\n3.0\n3.0\n3.0\n6.0\n6.0\n2.0\n6.0\n8.0\n8.0\n2.0\n2.0\n5')"
if [[ "$gpu_compute_output" != "$gpu_compute_expected" ]]; then
    echo "unexpected fake-GPU compute output:" >&2
    printf '%s\n' "$gpu_compute_output" >&2
    exit 1
fi

cat > "$TMP/gpu-neural-unary.qui" <<'QUI'
tensor<float> value = tensor.ones<float>([1], gpu = 0)
neural<float> exponential = neural.exponential(neural.track(value))
neural<float> restored = neural.logarithm(exponential)
float result = restored.untrack().reshape([]).item()
print(result > 0.999999999 and result < 1.000000001)
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-neural-unary.qui")" != "true" ]]; then
    echo "unexpected fake-GPU float64 neural exp/log result" >&2
    exit 1
fi

cat > "$TMP/gpu-log-domain.qui" <<'QUI'
tensor<float32> value = tensor.zeros<float32>([1], gpu = 0)
neural<float32> invalid = neural.logarithm(neural.track(value))
print(invalid.untrack().reshape([]).item())
QUI
expect_runtime_error "$TMP/gpu-log-domain.qui" "logarithm requires finite positive values"

cat > "$TMP/gpu-uninitialized.qui" <<'QUI'
tensor<int> value = tensor<int>([1], gpu = 0)
print(value[0].item())
QUI
expect_runtime_error "$TMP/gpu-uninitialized.qui" "uninitialized"

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
tensor<int32> rem_i32 = (tensor.ones<int32>([2], gpu = 0) * int32(7)) % int32(4)
tensor<int16> neg_i16 = -tensor.ones<int16>([2], gpu = 0)
tensor<int> i64 = tensor.ones<int>([2], gpu = 0) + 5
tensor<uint8> u8 = tensor.ones<uint8>([2], gpu = 0) + uint8(6)
tensor<uint16> u16 = tensor.ones<uint16>([2], gpu = 0) * uint16(7)
tensor<uint32> u32 = tensor.ones<uint32>([2], gpu = 0) + uint32(8)
tensor<uint64> u64 = tensor.ones<uint64>([2], gpu = 0) + uint64(9)

print(i8.cpu()[0].item())
print(i16.cpu()[0].item())
print(i32.cpu()[0].item())
print(rem_i32.cpu()[0].item())
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
integer_expected="$(printf '3\n3\n-3\n3\n-1\n6\n7\n7\n9\n10\n1\n3\n2\n18\n-3\n10\n3.0')"
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

cat > "$TMP/unsigned-underflow.qui" <<'QUI'
tensor<uint8> value = tensor.zeros<uint8>([1], gpu = 0)
tensor<uint8> invalid = value - uint8(1)
print(invalid[0].item())
QUI
expect_runtime_error "$TMP/unsigned-underflow.qui" "tensor integer arithmetic overflow"

cat > "$TMP/integer-div-zero.qui" <<'QUI'
tensor<int32> value = tensor.ones<int32>([1], gpu = 0)
tensor<int32> invalid = value / int32(0)
print(invalid[0].item())
QUI
expect_runtime_error "$TMP/integer-div-zero.qui" "invalid tensor division/remainder or integer overflow"

cat > "$TMP/integer-min-div-negative-one.qui" <<'QUI'
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(-128)
tensor<int8> invalid = value / int8(-1)
print(invalid[0].item())
QUI
expect_runtime_error "$TMP/integer-min-div-negative-one.qui" "invalid tensor division/remainder or integer overflow"

cat > "$TMP/integer-min-remainder-negative-one.qui" <<'QUI'
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(-128)
tensor<int8> remainder = value % int8(-1)
print(remainder[0].item())
QUI
if [[ "$("$QUIDRA" run "$TMP/integer-min-remainder-negative-one.qui")" != "0" ]]; then
    echo "fake-GPU signed min % -1 must match CPU semantics" >&2
    exit 1
fi

cat > "$TMP/integer-cast-range.qui" <<'QUI'
tensor<int16> source = tensor.ones<int16>([1], gpu = 0) * int16(300)
tensor<int8> invalid = int8(source)
print(invalid[0].item())
QUI
expect_runtime_error "$TMP/integer-cast-range.qui" "tensor cast is unsupported or a value is outside the target range"

cat > "$TMP/neural-all-reduce-sum.qui" <<'QUI'
tensor<float32> first = tensor.ones<float32>([2], gpu = 0)
tensor<float32> second = tensor.ones<float32>([2], gpu = 1) * float32(2)
tensor<float32>[] values = [first, second]
neural.all_reduce_sum(&values)
print(values[0].cpu()[0].item())
print(values[1].cpu()[1].item())
QUI
all_reduce_output="$("$QUIDRA" run "$TMP/neural-all-reduce-sum.qui")"
if [[ "$all_reduce_output" != "$(printf '3.0\n3.0')" ]]; then
    echo "unexpected fake-GPU neural.all_reduce_sum output:" >&2
    printf '%s\n' "$all_reduce_output" >&2
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

cat > "$TMP/gpu-autograd-fanin.qui" <<'QUI'
class Model
    neural.Parameter<float32> value

Model model = Model(
    value = neural.Parameter<float32>(
        value = tensor.ones<float32>([1], gpu = 0)
    )
)
neural<float32> first_track = model.value.track()
neural<float32> second_track = model.value.track()
neural<float32> first = first_track * first_track
neural<float32> second = second_track * second_track
neural<float32> loss = neural.mean(first + second)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
float32 updated = model.value.raw().cpu()[0].item()
print(updated > float32(0.5999) and updated < float32(0.6001))
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-autograd-fanin.qui")" != "true" ]]; then
    echo "unexpected fake-GPU autograd fan-in result" >&2
    exit 1
fi

cat > "$TMP/gpu-autograd-div.qui" <<'QUI'
class DivModel
    neural.Parameter<float32> left
    neural.Parameter<float32> right

DivModel model = DivModel(
    left = neural.Parameter<float32>(
        value = tensor.ones<float32>([1], gpu = 0) * float32(2)
    ),
    right = neural.Parameter<float32>(
        value = tensor.ones<float32>([1], gpu = 0) * float32(4)
    )
)
neural<float32> quotient = model.left.track() / model.right.track()
neural<float32> loss = neural.mean(quotient)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
float32 left_value = model.left.raw().cpu()[0].item()
float32 right_value = model.right.raw().cpu()[0].item()
print(left_value > float32(1.9749) and left_value < float32(1.9751))
print(right_value > float32(4.0124) and right_value < float32(4.0126))
QUI
div_output="$("$QUIDRA" run "$TMP/gpu-autograd-div.qui")"
if [[ "$div_output" != "$(printf 'true\ntrue')" ]]; then
    echo "unexpected fake-GPU autograd division result:" >&2
    printf '%s\n' "$div_output" >&2
    exit 1
fi


cat > "$TMP/gpu-autograd-scalar.qui" <<'QUI'
class ScalarModel
    neural.Parameter<float32> value

ScalarModel model = ScalarModel(
    value = neural.Parameter<float32>(
        value = tensor.ones<float32>([1], gpu = 0) * float32(2)
    )
)
neural<float32> x = model.value.track()
neural<float32> transformed = (float32(5) - x * float32(3)) / float32(2)
neural<float32> reciprocal = float32(8) / x
neural<float32> loss = neural.mean(transformed + reciprocal)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
float32 updated = model.value.raw().cpu()[0].item()
print(updated > float32(2.3499) and updated < float32(2.3501))
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-autograd-scalar.qui")" != "true" ]]; then
    echo "unexpected fake-GPU scalar autograd result" >&2
    exit 1
fi

echo "fake GPU placement contracts: ok"
