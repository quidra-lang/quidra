#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/reduction_helpers.qui" <<'QUI'
int element_count(tensor<float32> value)
    int count = 1
    for extent in value.shape()
        count = count * extent
    return count

tensor<float32> sum(tensor<float32> value)
    int count = element_count(value)
    if count == 0
        if value.device() >= 0
            return tensor.zeros<float32>([], gpu = value.device())
        return tensor.zeros<float32>([])
    tensor<float32> result = value.gather([0], [])
    for index in range(1, count)
        result = result + value.gather([index], [])
    return result

tensor<float32> mean(tensor<float32> value)
    int count = element_count(value)
    if count == 0
        error("test mean requires at least one element")
    return sum(value) / float32(count)
QUI

# This environment variable is recognized only when Quidra was compiled with
# QUIDRA_ENABLE_TEST_GPU_BACKEND. Production builds contain no fake backend.
export QUIDRA_TEST_FAKE_GPU_COUNT=2

cat > "$TMP/gpu-device-metadata.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> value = tensor.ones<float32>([1], gpu = 0)
print(value.device() == 0)
print(NL)
print(value.cpu().device() == -1)
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/gpu-device-metadata.qui")" == "$(printf 'true\ntrue')" ]]


gpu_info="$("$QUIDRA" gpu)"
grep -Fq "GPU 0" <<<"$gpu_info"
grep -Fq "GPU 1" <<<"$gpu_info"
grep -Fq "backend: TEST" <<<"$gpu_info"
grep -Fq "status: supported" <<<"$gpu_info"

cat > "$TMP/explicit-sync.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
gpu.sync(0)
gpu.sync(index = 1)
print("sync-ok")
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/explicit-sync.qui")" != "sync-ok" ]]; then
    echo "explicit gpu.sync contract failed" >&2
    exit 1
fi

cat > "$TMP/time-sync-mode.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
time.Instant default_start = time.now()
time.Duration default_elapsed = time.since(default_start)
print(default_elapsed.seconds() >= 0.0)
print(NL)

time.Instant async_start = time.now(sync = false)
time.Duration async_elapsed = time.since(async_start, sync = false)
print(async_elapsed.seconds() >= 0.0)
print(NL)

time.Instant synced_start = time.now(sync = true)
time.Duration synced_elapsed = time.since(synced_start, sync = true)
print(synced_elapsed.seconds() >= 0.0)
print(NL)
QUI
time_sync_output="$("$QUIDRA" run "$TMP/time-sync-mode.qui")"
if [[ "$time_sync_output" != "$(printf 'true\ntrue\ntrue')" ]]; then
    echo "unexpected time synchronization-mode output:" >&2
    printf '%s\n' "$time_sync_output" >&2
    exit 1
fi

cat > "$TMP/invalid-sync-device.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
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
import reductions = "./reduction_helpers.qui"
tensor<int> cpu = tensor.zeros<int>([3])
cpu[0] = 10
cpu[1] = 20
cpu[2] = 30

tensor<int> gpu0 = cpu.gpu(0)
tensor<int> same_gpu_copy = gpu0.gpu(0)
tensor<int> gpu1 = same_gpu_copy.gpu(1)
tensor<int> roundtrip = gpu1.cpu()

print(roundtrip[0].item())
print(NL)
print(roundtrip[1].item())
print(NL)
print(roundtrip[2].item())
print(NL)

tensor<int> direct_ones = tensor.ones<int>([2], gpu = 1)
tensor<int> ones_cpu = direct_ones.cpu()
print(ones_cpu[0].item())
print(NL)
print(ones_cpu[1].item())
print(NL)

tensor<int> direct_zeros = tensor.zeros<int>([2], gpu = 0)
tensor<int> zeros_cpu = direct_zeros.cpu()
print(zeros_cpu[0].item())
print(NL)
print(zeros_cpu[1].item())
print(NL)

tensor<int> reshaped_gpu = direct_ones.reshape([1, 2])
tensor<int> reshaped_cpu = reshaped_gpu.cpu()
print(reshaped_cpu.shape()[0])
print(NL)
print(reshaped_cpu.shape()[1])
print(NL)

tensor<int> contiguous_gpu = direct_ones.contiguous()
tensor<int> contiguous_cpu = contiguous_gpu.cpu()
print(contiguous_cpu[0].item())
print(NL)
QUI

transfer_output="$("$QUIDRA" run "$TMP/transfers.qui")"
transfer_expected="$(printf '10\n20\n30\n1\n1\n0\n0\n1\n2\n1')"
if [[ "$transfer_output" != "$transfer_expected" ]]; then
    echo "unexpected fake-GPU transfer output:" >&2
    printf '%s\n' "$transfer_output" >&2
    exit 1
fi

cat > "$TMP/gpu-copy-on-write.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int> original = tensor.ones<int>([2], gpu = 0)
tensor<int> copied = original
print(&original != &copied)
print(NL)
copied[0] = 9
print(original.cpu()[0].item())
print(NL)
print(copied.cpu()[0].item())
print(NL)
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
import reductions = "./reduction_helpers.qui"
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Instant default_start = time.now()
time.Duration default_elapsed = time.since(default_start)
time.Instant explicit_start = time.now(sync = false)
time.Duration explicit_elapsed = time.since(explicit_start, sync = false)
print(default_elapsed.seconds() >= 0.0 and explicit_elapsed.seconds() >= 0.0)
print(NL)
QUI
time_async_output="$(QUIDRA_TEST_FAKE_GPU_SYNC_FAIL=1 "$QUIDRA" run "$TMP/time-async-does-not-sync.qui")"
if [[ "$time_async_output" != "true" ]]; then
    echo "time sync=false unexpectedly synchronized fake GPU" >&2
    printf '%s\n' "$time_async_output" >&2
    exit 1
fi

cat > "$TMP/time-now-syncs-when-requested.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Instant synchronized = time.now(sync = true)
print("unreachable")
print(NL)
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
import reductions = "./reduction_helpers.qui"
time.Instant start = time.now(sync = false)
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Duration synchronized = time.since(start, sync = true)
print(synchronized.seconds())
print(NL)
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
import reductions = "./reduction_helpers.qui"
tensor<float32> used_gpu = tensor.ones<float32>([1], gpu = 0)
time.Instant start = time.now(sync = true)
time.Duration elapsed = time.since(start, sync = true)
print(elapsed.seconds() >= 0.0)
print(NL)
QUI
time_used_devices_output="$(QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX=1 "$QUIDRA" run "$TMP/time-sync-used-devices-only.qui")"
if [[ "$time_used_devices_output" != "true" ]]; then
    echo "synchronized timing touched an unused fake GPU" >&2
    printf '%s\n' "$time_used_devices_output" >&2
    exit 1
fi

cat > "$TMP/fake-sync-consumes-validation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(127)
tensor<int8> invalid = value + int8(1)
gpu.sync(0)
print("unreachable")
print(NL)
QUI
expect_runtime_error "$TMP/fake-sync-consumes-validation.qui" "tensor integer arithmetic overflow"
if [[ -s "$TMP/fake-sync-consumes-validation.qui.out" ]]; then
    echo "fake GPU synchronization did not stop at deferred validation failure" >&2
    cat "$TMP/fake-sync-consumes-validation.qui.out" >&2
    exit 1
fi

cat > "$TMP/cpu-gpu-mismatch.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> cpu = tensor.ones<float32>([2])
tensor<float32> gpu_value = tensor.ones<float32>([2], gpu = 0)
tensor<float32> invalid = cpu + gpu_value
print(invalid.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/cpu-gpu-mismatch.qui" "tensor operands are on different devices"

cat > "$TMP/gpu-gpu-mismatch.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> gpu0 = tensor.ones<float32>([2], gpu = 0)
tensor<float32> gpu1 = tensor.ones<float32>([2], gpu = 1)
tensor<float32> invalid = gpu0 + gpu1
print(invalid.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/gpu-gpu-mismatch.qui" "tensor operands are on different devices"

cat > "$TMP/gpu-compute.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> left = tensor.ones<float32>([2], gpu = 0)
tensor<float32> right = tensor.ones<float32>([2], gpu = 0)
tensor<float32> added = left + right
tensor<float32> scaled = added * 2.0
tensor<float32> reversed = 10.0 - scaled
tensor<float32> divided = reversed / 2.0
tensor<float32> negated = -left

print(added[0].item())
print(NL)
print(scaled[1].item())
print(NL)
print(divided[0].item())
print(NL)
print(negated[0].item())
print(NL)

tensor<float32> compare_high = tensor.ones<float32>([2], gpu = 0) * 2.0
print((left == right).all())
print(NL)
print((left != compare_high).any())
print(NL)
print((left < compare_high).all())
print(NL)
print((left <= compare_high).all())
print(NL)
print((left > compare_high).any())
print(NL)
print((compare_high >= left).all())
print(NL)
tensor<float32> compare_mixed = tensor.ones<float32>([2], gpu = 0)
compare_mixed[1] = 2.0
print((left != compare_mixed).any())
print(NL)

tensor<float32> compare_matrix = tensor.ones<float32>([2, 3], gpu = 0)
tensor<float32> compare_view_a = compare_matrix[0:2, 1:3]
tensor<float32> compare_view_b = compare_matrix[0:2, 1:3]
print((compare_view_a == compare_view_b).all())
print(NL)

tensor<int> values = tensor.zeros<int>([3], gpu = 0)
values[0] = 1
values[1] = 2
values[2] = 3
tensor<float> converted = float(values)
print(converted[2].item())
print(NL)

tensor<float32> scalar_base = tensor.ones<float32>([1], gpu = 0) * 4.0
print((scalar_base + 2.0)[0].item())
print(NL)
print((2.0 + scalar_base)[0].item())
print(NL)
print((scalar_base - 2.0)[0].item())
print(NL)
print((10.0 - scalar_base)[0].item())
print(NL)
print((scalar_base * 2.0)[0].item())
print(NL)
print((2.0 * scalar_base)[0].item())
print(NL)
print((scalar_base / 2.0)[0].item())
print(NL)
print((8.0 / scalar_base)[0].item())
print(NL)

tensor<int> direct = tensor<int>([2], gpu = 0)
direct[0] = 4
direct[1] = 5
print(direct[1].item())
print(NL)
QUI

gpu_compute_output="$("$QUIDRA" run "$TMP/gpu-compute.qui")"
gpu_compute_expected="$(printf '2.0\n4.0\n3.0\n-1.0\ntrue\ntrue\ntrue\ntrue\nfalse\ntrue\ntrue\ntrue\n3.0\n6.0\n6.0\n2.0\n6.0\n8.0\n8.0\n2.0\n2.0\n5')"
if [[ "$gpu_compute_output" != "$gpu_compute_expected" ]]; then
    echo "unexpected fake-GPU compute output:" >&2
    printf '%s\n' "$gpu_compute_output" >&2
    exit 1
fi

cat > "$TMP/gpu-scatter.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> values = tensor.ones<float32>([3], gpu = 0).track()
tensor<float32> scattered = values.scatter([0, 0, 2], [4])
tensor<float32> host = scattered.untrack().cpu()
print(host[0].item() == float32(2))
print(NL)
print(host[1].item() == float32(0))
print(NL)
print(host[2].item() == float32(1))
print(NL)
print(host[3].item() == float32(0))
print(NL)
reductions.mean(scattered).backward(&values)
tensor<float32> gradient = values.grad.cpu()
print(gradient[0].item() == float32(0.25))
print(NL)
print(gradient[1].item() == float32(0.25))
print(NL)
print(gradient[2].item() == float32(0.25))
print(NL)

tensor<int> integer_values = tensor.zeros<int>([3], gpu = 0)
integer_values[0] = 1
integer_values[1] = 2
integer_values[2] = 3
tensor<int> integer_scattered = integer_values.scatter([0, 0, 2], [4]).cpu()
print(integer_scattered[0].item() == 3)
print(NL)
print(integer_scattered[1].item() == 0)
print(NL)
print(integer_scattered[2].item() == 3)
print(NL)
print(integer_scattered[3].item() == 0)
print(NL)
QUI
gpu_scatter_output="$("$QUIDRA" run "$TMP/gpu-scatter.qui")"
gpu_scatter_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$gpu_scatter_output" != "$gpu_scatter_expected" ]]; then
    echo "unexpected fake-GPU tensor scatter output:" >&2
    printf '%s\n' "$gpu_scatter_output" >&2
    exit 1
fi

cat > "$TMP/gpu-uninitialized.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int> value = tensor<int>([1], gpu = 0)
print(value[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/gpu-uninitialized.qui" "uninitialized"

cat > "$TMP/gpu-view.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
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
print(NL)
print(dense.shape()[1])
print(NL)
print(dense[0, 0].item())
print(NL)
print(dense[1, 1].item())
print(NL)

tensor<int> direct_cpu = view.cpu()
print(direct_cpu[0, 0].item())
print(NL)
print(direct_cpu[1, 1].item())
print(NL)

tensor<int> cross_gpu = view.gpu(1)
tensor<int> cross_cpu = cross_gpu.cpu()
print(cross_cpu[0, 0].item())
print(NL)
print(cross_cpu[1, 1].item())
print(NL)

tensor<int><3, 2> transposed = value.transpose(0, 1)
print(transposed.shape()[0])
print(NL)
print(transposed.shape()[1])
print(NL)
print(transposed[2, 1].item())
print(NL)
print(transposed.is_contiguous())
print(NL)
tensor<int> transpose_dense = transposed.contiguous()
print(transpose_dense[2, 1].item())
print(NL)
QUI
gpu_view_output="$("$QUIDRA" run "$TMP/gpu-view.qui")"
gpu_view_expected="$(printf '2\n2\n2\n6\n2\n6\n2\n6\n3\n2\n6\nfalse\n6')"
if [[ "$gpu_view_output" != "$gpu_view_expected" ]]; then
    echo "unexpected fake-GPU view output:" >&2
    printf '%s\n' "$gpu_view_output" >&2
    exit 1
fi


cat > "$TMP/integer-dtypes.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
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
print(NL)
print(i16.cpu()[0].item())
print(NL)
print(i32.cpu()[0].item())
print(NL)
print(rem_i32.cpu()[0].item())
print(NL)
print(neg_i16.cpu()[0].item())
print(NL)
print(i64.cpu()[0].item())
print(NL)
print(u8.cpu()[0].item())
print(NL)
print(u16.cpu()[0].item())
print(NL)
print(u32.cpu()[0].item())
print(NL)
print(u64.cpu()[0].item())
print(NL)

tensor<int8> cast_source = tensor.ones<int8>([2], gpu = 0)
tensor<uint16> casted = uint16(cast_source)
print(casted.cpu()[1].item())
print(NL)

QUI

integer_output="$("$QUIDRA" run "$TMP/integer-dtypes.qui")"
integer_expected="$(printf '3\n3\n-3\n3\n-1\n6\n7\n7\n9\n10\n1')"
if [[ "$integer_output" != "$integer_expected" ]]; then
    echo "unexpected fake-GPU integer dtype output:" >&2
    printf '%s\n' "$integer_output" >&2
    exit 1
fi

cat > "$TMP/integer-overflow.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(127)
tensor<int8> invalid = value + int8(1)
print(invalid.cpu()[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/integer-overflow.qui" "tensor integer arithmetic overflow"

cat > "$TMP/unsigned-underflow.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<uint8> value = tensor.zeros<uint8>([1], gpu = 0)
tensor<uint8> invalid = value - uint8(1)
print(invalid[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/unsigned-underflow.qui" "tensor integer arithmetic overflow"

cat > "$TMP/integer-div-zero.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int32> value = tensor.ones<int32>([1], gpu = 0)
tensor<int32> invalid = value / int32(0)
print(invalid[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/integer-div-zero.qui" "invalid tensor division/remainder or integer overflow"

cat > "$TMP/integer-min-div-negative-one.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(-128)
tensor<int8> invalid = value / int8(-1)
print(invalid[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/integer-min-div-negative-one.qui" "invalid tensor division/remainder or integer overflow"

cat > "$TMP/integer-min-remainder-negative-one.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(-128)
tensor<int8> remainder = value % int8(-1)
print(remainder[0].item())
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/integer-min-remainder-negative-one.qui")" != "0" ]]; then
    echo "fake-GPU signed min % -1 must match CPU semantics" >&2
    exit 1
fi

cat > "$TMP/integer-cast-range.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int16> source = tensor.ones<int16>([1], gpu = 0) * int16(300)
tensor<int8> invalid = int8(source)
print(invalid[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/integer-cast-range.qui" "numeric cast outside destination range"


cat > "$TMP/tracked-transfer-guard.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> cpu_value = tensor.ones<float32>([1]).track()
tensor<float32> invalid_gpu = cpu_value.gpu(0)
print(invalid_gpu.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/tracked-transfer-guard.qui" "gpu() on a tracked tensor requires explicit untrack() first"

cat > "$TMP/tracked-cpu-transfer-guard.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> gpu_value = tensor.ones<float32>([1], gpu = 0).track()
tensor<float32> invalid_cpu = gpu_value.cpu()
print(invalid_cpu.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/tracked-cpu-transfer-guard.qui" "cpu() on a tracked tensor requires explicit untrack() first"

cat > "$TMP/gpu-autograd-fanin.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> source = tensor.ones<float32>([1], gpu = 0)
tensor<float32> first_track = source.track()
tensor<float32> second_track = source.track()
tensor<float32> first = first_track * first_track
tensor<float32> second = second_track * second_track
tensor<float32> loss = reductions.mean((first + second))
loss.backward(&source)
float32 gradient = source.grad.cpu()[0].item()
print(gradient > float32(3.9999) and gradient < float32(4.0001))
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-autograd-fanin.qui")" != "true" ]]; then
    echo "unexpected fake-GPU autograd fan-in result" >&2
    exit 1
fi

cat > "$TMP/gpu-autograd-div.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> left = tensor.ones<float32>([1], gpu = 0) * float32(2)
tensor<float32> right = tensor.ones<float32>([1], gpu = 0) * float32(4)
tensor<float32> left_tracked = left.track()
tensor<float32> right_tracked = right.track()
tensor<float32> quotient = left_tracked / right_tracked
reductions.mean(quotient).backward(&left, &right)
float32 left_grad = left.grad.cpu()[0].item()
float32 right_grad = right.grad.cpu()[0].item()
print(left_grad > float32(0.2499) and left_grad < float32(0.2501))
print(NL)
print(right_grad > float32(-0.1251) and right_grad < float32(-0.1249))
print(NL)
QUI
div_output="$("$QUIDRA" run "$TMP/gpu-autograd-div.qui")"
if [[ "$div_output" != "$(printf 'true\ntrue')" ]]; then
    echo "unexpected fake-GPU autograd division result:" >&2
    printf '%s\n' "$div_output" >&2
    exit 1
fi

cat > "$TMP/gpu-autograd-broadcast.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> left = (
    tensor.ones<float32>([2, 1], gpu = 0) * float32(2)
).track()
tensor<float32> right = tensor.ones<float32>([2, 3], gpu = 0).track()
tensor<float32> output = left * right
reductions.mean(output).backward(&left, &right)
tensor<float32> left_grad = left.grad.cpu()
tensor<float32> right_grad = right.grad.cpu()
print(left_grad[0, 0].item() == float32(0.5))
print(NL)
print(left_grad[1, 0].item() == float32(0.5))
print(NL)
print(right_grad[0, 0].item() > float32(0.3333) and right_grad[0, 0].item() < float32(0.3334))
print(NL)
print(right_grad[1, 2].item() > float32(0.3333) and right_grad[1, 2].item() < float32(0.3334))
print(NL)
QUI
broadcast_output="$("$QUIDRA" run "$TMP/gpu-autograd-broadcast.qui")"
if [[ "$broadcast_output" != "$(printf 'true\ntrue\ntrue\ntrue')" ]]; then
    echo "unexpected fake-GPU broadcast autograd result:" >&2
    printf '%s\n' "$broadcast_output" >&2
    exit 1
fi

cat > "$TMP/gpu-autograd-scalar.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<float32> source = tensor.ones<float32>([1], gpu = 0) * float32(2)
tensor<float32> x = source.track()
tensor<float32> transformed = (float32(5) - x * float32(3)) / float32(2)
tensor<float32> reciprocal = float32(8) / x
tensor<float32> loss = reductions.mean((transformed + reciprocal))
loss.backward(&source)
float32 gradient = source.grad.cpu()[0].item()
print(gradient > float32(-3.5001) and gradient < float32(-3.4999))
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-autograd-scalar.qui")" != "true" ]]; then
    echo "unexpected fake-GPU scalar autograd result" >&2
    exit 1
fi

echo "fake GPU placement contracts: ok"