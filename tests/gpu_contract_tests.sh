#!/usr/bin/env bash
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

cat > "$TMP/gpu-device-metadata.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> value = tensor.ones<real32>([1], gpu = 0)
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
tensor<int64> cpu = tensor.zeros<int64>([3])
cpu[0] = 10
cpu[1] = 20
cpu[2] = 30

tensor<int64> gpu0 = cpu.gpu(0)
tensor<int64> same_gpu_copy = gpu0.gpu(0)
tensor<int64> gpu1 = same_gpu_copy.gpu(1)
tensor<int64> roundtrip = gpu1.cpu()

print(roundtrip[0].item())
print(NL)
print(roundtrip[1].item())
print(NL)
print(roundtrip[2].item())
print(NL)

tensor<int64> direct_ones = tensor.ones<int64>([2], gpu = 1)
tensor<int64> ones_cpu = direct_ones.cpu()
print(ones_cpu[0].item())
print(NL)
print(ones_cpu[1].item())
print(NL)

tensor<int64> direct_zeros = tensor.zeros<int64>([2], gpu = 0)
tensor<int64> zeros_cpu = direct_zeros.cpu()
print(zeros_cpu[0].item())
print(NL)
print(zeros_cpu[1].item())
print(NL)

tensor<int64> reshaped_gpu = direct_ones.reshape([1, 2])
tensor<int64> reshaped_cpu = reshaped_gpu.cpu()
print(reshaped_cpu.shape()[0])
print(NL)
print(reshaped_cpu.shape()[1])
print(NL)

tensor<int64> contiguous_gpu = direct_ones.contiguous()
tensor<int64> contiguous_cpu = contiguous_gpu.cpu()
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
tensor<int64> original = tensor.ones<int64>([2], gpu = 0)
tensor<int64> copied = original
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
tensor<real32> used_gpu = tensor.ones<real32>([1], gpu = 0)
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
tensor<real32> used_gpu = tensor.ones<real32>([1], gpu = 0)
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

# The hook counts as set when it is empty.
set +e
QUIDRA_TEST_FAKE_GPU_SYNC_FAIL= "$QUIDRA" run "$TMP/time-now-syncs-when-requested.qui" >"$TMP/sync-fail-empty.out" 2>"$TMP/sync-fail-empty.err"
sync_fail_empty_status=$?
set -e
if [[ $sync_fail_empty_status -ne 101 ]] ||
    ! grep -Fq "test-only fake GPU synchronization failure" "$TMP/sync-fail-empty.err"; then
    echo "QUIDRA_TEST_FAKE_GPU_SYNC_FAIL set to the empty string did not fail synchronization" >&2
    exit 1
fi

cat > "$TMP/time-since-syncs-when-requested.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
time.Instant start = time.now(sync = false)
tensor<real32> used_gpu = tensor.ones<real32>([1], gpu = 0)
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
tensor<real32> used_gpu = tensor.ones<real32>([1], gpu = 0)
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
set +e
QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX=0 "$QUIDRA" run "$TMP/time-sync-used-devices-only.qui" >"$TMP/sync-fail-index.out" 2>"$TMP/sync-fail-index.err"
sync_fail_index_status=$?
set -e
if [[ $sync_fail_index_status -ne 101 ]] ||
    ! grep -Fq "test-only fake GPU synchronization failure" "$TMP/sync-fail-index.err"; then
    echo "QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX=0 did not fail the synchronization of GPU 0" >&2
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
tensor<real32> cpu = tensor.ones<real32>([2])
tensor<real32> gpu_value = tensor.ones<real32>([2], gpu = 0)
tensor<real32> invalid = cpu + gpu_value
print(invalid.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/cpu-gpu-mismatch.qui" "tensor operands are on different devices"

cat > "$TMP/gpu-gpu-mismatch.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> gpu0 = tensor.ones<real32>([2], gpu = 0)
tensor<real32> gpu1 = tensor.ones<real32>([2], gpu = 1)
tensor<real32> invalid = gpu0 + gpu1
print(invalid.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/gpu-gpu-mismatch.qui" "tensor operands are on different devices"

cat > "$TMP/gpu-compute.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> left = tensor.ones<real32>([2], gpu = 0)
tensor<real32> right = tensor.ones<real32>([2], gpu = 0)
tensor<real32> added = left + right
tensor<real32> scaled = added * 2.0
tensor<real32> reversed = 10.0 - scaled
tensor<real32> divided = reversed / 2.0
tensor<real32> negated = -left

print(added[0].item())
print(NL)
print(scaled[1].item())
print(NL)
print(divided[0].item())
print(NL)
print(negated[0].item())
print(NL)

tensor<real32> compare_high = tensor.ones<real32>([2], gpu = 0) * 2.0
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
tensor<real32> compare_mixed = tensor.ones<real32>([2], gpu = 0)
compare_mixed[1] = 2.0
print((left != compare_mixed).any())
print(NL)

tensor<real32> compare_matrix = tensor.ones<real32>([2, 3], gpu = 0)
tensor<real32> compare_view_a = compare_matrix[0:2, 1:3]
tensor<real32> compare_view_b = compare_matrix[0:2, 1:3]
print((compare_view_a == compare_view_b).all())
print(NL)

tensor<int64> values = tensor.zeros<int64>([3], gpu = 0)
values[0] = 1
values[1] = 2
values[2] = 3
tensor<real64> converted = real64(values)
print(converted[2].item())
print(NL)

tensor<real32> scalar_base = tensor.ones<real32>([1], gpu = 0) * 4.0
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

tensor<int64> direct = tensor<int64>([2], gpu = 0)
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
tensor<real32> values = tensor.ones<real32>([3], gpu = 0).track()
tensor<real32> scattered = values.scatter([0, 0, 2], [4])
tensor<real32> host = scattered.untrack().cpu()
print(host[0].item() == real32(2))
print(NL)
print(host[1].item() == real32(0))
print(NL)
print(host[2].item() == real32(1))
print(NL)
print(host[3].item() == real32(0))
print(NL)
reductions.mean(scattered).backward(&values)
tensor<real32> gradient = values.grad.cpu()
print(gradient[0].item() == real32(0.25))
print(NL)
print(gradient[1].item() == real32(0.25))
print(NL)
print(gradient[2].item() == real32(0.25))
print(NL)

tensor<int64> integer_values = tensor.zeros<int64>([3], gpu = 0)
integer_values[0] = 1
integer_values[1] = 2
integer_values[2] = 3
tensor<int64> integer_scattered = integer_values.scatter([0, 0, 2], [4]).cpu()
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
tensor<int64> value = tensor<int64>([1], gpu = 0)
print(value[0].item())
print(NL)
QUI
expect_runtime_error "$TMP/gpu-uninitialized.qui" "uninitialized"

cat > "$TMP/gpu-view.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<int64> value = tensor.zeros<int64>([2, 3], gpu = 0)
value[0, 0] = 1
value[0, 1] = 2
value[0, 2] = 3
value[1, 0] = 4
value[1, 1] = 5
value[1, 2] = 6
tensor<int64> view = value[0:2, 1:3]
tensor<int64> dense = view.contiguous()
print(dense.shape()[0])
print(NL)
print(dense.shape()[1])
print(NL)
print(dense[0, 0].item())
print(NL)
print(dense[1, 1].item())
print(NL)

tensor<int64> direct_cpu = view.cpu()
print(direct_cpu[0, 0].item())
print(NL)
print(direct_cpu[1, 1].item())
print(NL)

tensor<int64> cross_gpu = view.gpu(1)
tensor<int64> cross_cpu = cross_gpu.cpu()
print(cross_cpu[0, 0].item())
print(NL)
print(cross_cpu[1, 1].item())
print(NL)

tensor<int64><3, 2> transposed = value.transpose(0, 1)
print(transposed.shape()[0])
print(NL)
print(transposed.shape()[1])
print(NL)
print(transposed[2, 1].item())
print(NL)
print(transposed.is_contiguous())
print(NL)
tensor<int64> transpose_dense = transposed.contiguous()
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
tensor<int64> i64 = tensor.ones<int64>([2], gpu = 0) + 5
tensor<nat8> u8 = tensor.ones<nat8>([2], gpu = 0) + nat8(6)
tensor<nat16> u16 = tensor.ones<nat16>([2], gpu = 0) * nat16(7)
tensor<nat32> u32 = tensor.ones<nat32>([2], gpu = 0) + nat32(8)
tensor<nat64> u64 = tensor.ones<nat64>([2], gpu = 0) + nat64(9)

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
tensor<nat16> casted = nat16(cast_source)
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
tensor<nat8> value = tensor.zeros<nat8>([1], gpu = 0)
tensor<nat8> invalid = value - nat8(1)
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
expect_runtime_error "$TMP/integer-cast-range.qui" "numeric conversion out of range: tensor element cannot be represented as int8"


cat > "$TMP/tracked-transfer-guard.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> cpu_value = tensor.ones<real32>([1]).track()
tensor<real32> invalid_gpu = cpu_value.gpu(0)
print(invalid_gpu.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/tracked-transfer-guard.qui" "gpu() on a tracked tensor requires explicit untrack() first"

cat > "$TMP/tracked-cpu-transfer-guard.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> gpu_value = tensor.ones<real32>([1], gpu = 0).track()
tensor<real32> invalid_cpu = gpu_value.cpu()
print(invalid_cpu.shape()[0])
print(NL)
QUI
expect_runtime_error "$TMP/tracked-cpu-transfer-guard.qui" "cpu() on a tracked tensor requires explicit untrack() first"

cat > "$TMP/gpu-autograd-fanin.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([1], gpu = 0)
tensor<real32> first_track = source.track()
tensor<real32> second_track = source.track()
tensor<real32> first = first_track * first_track
tensor<real32> second = second_track * second_track
tensor<real32> loss = reductions.mean((first + second))
loss.backward(&source)
real32 gradient = source.grad.cpu()[0].item()
print(gradient > real32(3.9999) and gradient < real32(4.0001))
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-autograd-fanin.qui")" != "true" ]]; then
    echo "unexpected fake-GPU autograd fan-in result" >&2
    exit 1
fi

cat > "$TMP/gpu-autograd-div.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> left = tensor.ones<real32>([1], gpu = 0) * real32(2)
tensor<real32> right = tensor.ones<real32>([1], gpu = 0) * real32(4)
tensor<real32> left_tracked = left.track()
tensor<real32> right_tracked = right.track()
tensor<real32> quotient = left_tracked / right_tracked
reductions.mean(quotient).backward(&left, &right)
real32 left_grad = left.grad.cpu()[0].item()
real32 right_grad = right.grad.cpu()[0].item()
print(left_grad > real32(0.2499) and left_grad < real32(0.2501))
print(NL)
print(right_grad > real32(-0.1251) and right_grad < real32(-0.1249))
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
tensor<real32> left = (
    tensor.ones<real32>([2, 1], gpu = 0) * real32(2)
).track()
tensor<real32> right = tensor.ones<real32>([2, 3], gpu = 0).track()
tensor<real32> output = left * right
reductions.mean(output).backward(&left, &right)
tensor<real32> left_grad = left.grad.cpu()
tensor<real32> right_grad = right.grad.cpu()
print(left_grad[0, 0].item() == real32(0.5))
print(NL)
print(left_grad[1, 0].item() == real32(0.5))
print(NL)
print(right_grad[0, 0].item() > real32(0.3333) and right_grad[0, 0].item() < real32(0.3334))
print(NL)
print(right_grad[1, 2].item() > real32(0.3333) and right_grad[1, 2].item() < real32(0.3334))
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
tensor<real32> source = tensor.ones<real32>([1], gpu = 0) * real32(2)
tensor<real32> x = source.track()
tensor<real32> transformed = (real32(5) - x * real32(3)) / real32(2)
tensor<real32> reciprocal = real32(8) / x
tensor<real32> loss = reductions.mean((transformed + reciprocal))
loss.backward(&source)
real32 gradient = source.grad.cpu()[0].item()
print(gradient > real32(-3.5001) and gradient < real32(-3.4999))
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/gpu-autograd-scalar.qui")" != "true" ]]; then
    echo "unexpected fake-GPU scalar autograd result" >&2
    exit 1
fi

# Materializing a GPU view gathers it into dense storage. A fully
# initialized source marks the whole output initialized in one step; a
# partially initialized source still propagates initialization per element.
cat > "$TMP/gpu-materialize-initialization.qui" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3], gpu = 0)
grid[0, 0] = real32(1)
grid[0, 1] = real32(2)
grid[0, 2] = real32(3)
grid[1, 0] = real32(4)
grid[1, 1] = real32(5)
grid[1, 2] = real32(6)

tensor<real32> dense = grid.transpose(0, 1).contiguous()
print(dense.is_contiguous() and dense[2, 1].item() == real32(6) and dense[0, 1].item() == real32(4))
print(NL)
tensor<real32> negated = -dense
print(negated.cpu()[1, 0].item() == real32(-2))
print(NL)
tensor<real32> host = grid.transpose(0, 1).cpu()
print(host[2, 0].item() == real32(3) and host[1, 1].item() == real32(5))
print(NL)
tensor<real32> stepped = -(grid.reshape([6])[1:6:2])
print(stepped.cpu()[2].item() == real32(-6))
print(NL)
tensor<real32> sum = grid.transpose(0, 1) + dense
print(sum.cpu()[2, 1].item() == real32(12))
print(NL)
print(real64(grid.transpose(0, 1)).cpu()[2, 1].item() == 6.0)
print(NL)

tensor<real32> partial = tensor<real32>([6], gpu = 0)
partial[0] = real32(1)
partial[2] = real32(3)
partial[3] = real32(4)
partial[4] = real32(5)
tensor<real32> even = partial[0:6:2].contiguous()
print((-even).cpu()[2].item() == real32(-5))
print(NL)
tensor<real32> mixed = partial[1:5:2].contiguous()
print(mixed[1].item() == real32(4))
print(NL)
QUI
materialize_output="$("$QUIDRA" run "$TMP/gpu-materialize-initialization.qui")"
if [[ "$materialize_output" != "$(for _ in {1..8}; do echo true; done)" ]]; then
    echo "unexpected fake-GPU materialization initialization result:" >&2
    printf '%s\n' "$materialize_output" >&2
    exit 1
fi

for uninitialized_use in 'tensor<real32> ignored = -mixed' 'real32 ignored = mixed[0].item()' 'tensor<real32> ignored = -partial[1:5:2]'; do
    cat > "$TMP/gpu-materialize-uninitialized.qui" <<QUI
tensor<real32> partial = tensor<real32>([6], gpu = 0)
partial[0] = real32(1)
partial[2] = real32(3)
partial[3] = real32(4)
partial[4] = real32(5)
tensor<real32> mixed = partial[1:5:2].contiguous()
$uninitialized_use
QUI
    expect_runtime_error "$TMP/gpu-materialize-uninitialized.qui" "uninitialized"
done

# Gathers mark their output initialized in one step when the source is fully
# initialized and propagate per element otherwise; broadcast operands are
# expanded into initialized temporaries.
cat > "$TMP/gpu-gather-initialization.qui" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3], gpu = 0)
grid[0, 0] = real32(1)
grid[0, 1] = real32(2)
grid[0, 2] = real32(3)
grid[1, 0] = real32(4)
grid[1, 1] = real32(5)
grid[1, 2] = real32(6)
tensor<real32> flat = grid.reshape([6])

tensor<real32> picked = flat[2:6].gather([3, 0], [2])
print((-picked).cpu()[0].item() == real32(-6) and picked[1].item() == real32(3))
print(NL)
tensor<real32> strided = grid.transpose(0, 1).gather([1, 4], [2]).cpu()
print(strided[0].item() == real32(4) and strided[1].item() == real32(3))
print(NL)
tensor<real32> broadcast = grid + grid[1:2, 0:3]
print((-broadcast).cpu()[0, 2].item() == real32(-9))
print(NL)

tensor<real32> partial = tensor<real32>([6], gpu = 0)
partial[0] = real32(1)
partial[2] = real32(3)
partial[4] = real32(5)
print((-partial.gather([0, 4], [2])).cpu()[1].item() == real32(-5))
print(NL)
tensor<real32> holes = partial.gather([0, 3], [2])
print(holes[0].item() == real32(1))
print(NL)
QUI
gather_initialization_output="$("$QUIDRA" run "$TMP/gpu-gather-initialization.qui")"
if [[ "$gather_initialization_output" != "$(for _ in {1..5}; do echo true; done)" ]]; then
    echo "unexpected fake-GPU gather initialization result:" >&2
    printf '%s\n' "$gather_initialization_output" >&2
    exit 1
fi

for uninitialized_use in 'real32 ignored = holes[1].item()' 'tensor<real32> ignored = -holes'; do
    cat > "$TMP/gpu-gather-uninitialized.qui" <<QUI
tensor<real32> partial = tensor<real32>([6], gpu = 0)
partial[0] = real32(1)
partial[2] = real32(3)
partial[4] = real32(5)
tensor<real32> holes = partial.gather([0, 3], [2])
$uninitialized_use
QUI
    expect_runtime_error "$TMP/gpu-gather-uninitialized.qui" "uninitialized"
done

# backward(track = true) on gpu(n): every symbolic gradient node holds a device
# value, a tracked gradient that arrives through a transpose is stored dense
# like the CPU's, and the second derivative matches the CPU (the fake
# backend's kernels are the CPU's arithmetic, so bitwise) and central
# differences.
cat > "$TMP/gpu-higher-order.qui" <<'QUI'
cli args
    int device = option(default = 0)
    bool bitwise = option(default = false)

// backward(track = true) on gpu(n) against the CPU and against finite
// differences. Every check prints one line ending in "true".

void report(string name, bool passed)
    print("{name} {passed}{NL}")

// Sum through tracked gather nodes, so that the graph exercises Gather and,
// in its backward, GatherBackward.
tensor<real32> total(tensor<real32> value)
    int count = 1
    for extent in value.shape()
        count = count * int(extent)
    tensor<real32> flat = value.reshape([nat(count)])
    tensor<real32> result = flat.gather([0], [])
    for index in range(1, count)
        result = result + flat.gather([index], [])
    return result

tensor<real32> pattern(int count, int seed)
    tensor<real32> value = tensor.zeros<real32>([nat(count)])
    for index in range(count)
        real32 step = real32((index * 7 + seed * 3) % 11)
        value[index] = step / real32(8) + real32(0.25)
    return value

tensor<real32> place(tensor<real32> value, int device)
    if device >= 0
        return value.gpu(nat(device))
    return value

// Element-wise |actual - expected| <= sqrt(atol^2 + (rtol * expected)^2):
// an absolute bound near zero and a relative one elsewhere. atol = rtol = 0
// requires bitwise-equal values.
bool close(tensor<real32> expected, tensor<real32> actual, real64 atol, real64 rtol)
    tensor<real32> a = expected.untrack().cpu()
    tensor<real32> b = actual.untrack().cpu()
    if a.shape() != b.shape()
        return false
    tensor<real32> difference = a - b
    real32 absolute = real32(atol)
    real32 relative = real32(rtol)
    return (difference * difference <= a * a * (relative * relative) + absolute * absolute).all()

// 0: binary + - * / with a shared operand, 1: scalar + - * / and power on
// both sides, 2: reshape, transpose, gather (repeated indices) and scatter.
tensor<real32> network(int which, tensor<real32> x, tensor<real32> w)
    if which == 0
        return x * x * w + x / w - w * x * x * x + (x - w) * (x + w)
    if which == 1
        tensor<real32> scaled = (x * real32(3) + real32(1)) * (real32(2) - x) / real32(4)
        return scaled + real32(5) / x + (x ^ real32(3)) - (real32(1) - x * real32(0.5))
    tensor<real32> view = (x * w).reshape([2, 3]).transpose(0, 1)
    tensor<real32> picked = view.gather([5, 0, 3, 3], [4])
    tensor<real32> spread = (x * x).scatter([0, 2, 2, 1, 0, 3], [4])
    return picked * spread * picked + spread

class Derivatives
    tensor<real32> first
    tensor<real32> second
    bool dense

// first = d loss / dx, second = d (first . probe) / dx = H probe
Derivatives derivatives(int which, int device)
    tensor<real32> x = place(pattern(6, 1), device).track()
    tensor<real32> w = place(pattern(6, 2), device)
    tensor<real32> output = network(which, x, w)
    tensor<real32> upstream = place(pattern(int(output.shape()[0]), 3), device)
    total(output * upstream).backward(&x, track = true)
    tensor<real32> first = x.grad
    Derivatives result
    result.first = first.untrack().cpu()
    x.clear_grad()
    total(first * place(pattern(6, 4), device)).backward(&x)
    result.second = x.grad.untrack().cpu()
    return result

// The first-order gradient at an untracked point (first-order engine only).
tensor<real32> gradient_at(int which, tensor<real32> point, int device)
    tensor<real32> x = place(point, device).track()
    tensor<real32> w = place(pattern(6, 2), device)
    tensor<real32> output = network(which, x, w)
    tensor<real32> upstream = place(pattern(int(output.shape()[0]), 3), device)
    total(output * upstream).backward(&x)
    return x.grad.untrack().cpu()

// Central difference of the first-order gradient along the probe.
tensor<real32> finite_hessian_probe(int which, int device)
    real32 step = real32(0.01)
    tensor<real32> probe = pattern(6, 4)
    tensor<real32> plus = gradient_at(which, pattern(6, 1) + probe * step, device)
    tensor<real32> minus = gradient_at(which, pattern(6, 1) - probe * step, device)
    return (plus - minus) / (step * real32(2))

// The finite-difference checks bound truncation error, not rounding:
// |d| <= 0.02 * sqrt(1 + expected^2).
bool case_passes(int which, int device, real64 atol, real64 rtol, real64 first_rtol)
    Derivatives host = derivatives(which, -1)
    Derivatives device_result = derivatives(which, device)
    bool first = close(host.first, device_result.first, atol, first_rtol)
    bool second = close(host.second, device_result.second, atol, rtol)
    bool host_fd = close(finite_hessian_probe(which, -1), host.second, 0.02, 0.02)
    bool gpu_fd = close(finite_hessian_probe(which, device), device_result.second, 0.02, 0.02)
    bool first_order = close(gradient_at(which, pattern(6, 1), device), device_result.first, atol, rtol)
    return first and second and host_fd and gpu_fd and first_order

// Cross-backend tolerances per quantity. Every quantity allows atol 1e-6 and
// rtol 1e-6 (about 8 float32 ulps): GPU kernels may round differently from
// the CPU, and bitwise equality is not a contract across GPU drivers and
// generations. The scalar first derivative goes through the division and
// power kernels, whose GPU versions may differ from the CPU by a few more
// ulps, and allows rtol 2e-6.
// --bitwise true (the fake GPU, whose kernels are the CPU's arithmetic)
// requires every quantity to be bitwise equal.
real64 atol = 0.000001
real64 rtol = 0.000001
real64 scalar_first_rtol = 0.000002
if args.bitwise
    atol = 0.0
    rtol = 0.0
    scalar_first_rtol = 0.0

report("gpu higher-order binary", case_passes(0, args.device, atol, rtol, rtol))
report("gpu higher-order scalar", case_passes(1, args.device, atol, rtol, scalar_first_rtol))
report("gpu higher-order gather view", case_passes(2, args.device, atol, rtol, rtol))

// A gradient that reaches its target through a single transpose is a strided
// view on gpu(n). The tracked x.grad is stored dense, as on the CPU, so it
// can be reshaped while tracked (contiguous() rejects tracked tensors), and
// its second derivative matches the CPU.
Derivatives transposed_derivatives(int device)
    tensor<real32> x = place(pattern(6, 1), device).reshape([2, 3]).track()
    tensor<real32> w = place(pattern(6, 2), device).reshape([3, 2])
    tensor<real32> view = x.transpose(0, 1)
    total(view * view * w).backward(&x, track = true)
    tensor<real32> first = x.grad
    Derivatives result
    result.dense = first.is_tracked() and first.is_contiguous()
    result.first = first.untrack().cpu()
    x.clear_grad()
    total(first.reshape([6]) * place(pattern(6, 4), device)).backward(&x)
    result.second = x.grad.untrack().cpu()
    return result

Derivatives layout_host = transposed_derivatives(-1)
Derivatives layout_device = transposed_derivatives(args.device)
bool layout_values = close(layout_host.first, layout_device.first, atol, rtol) and close(layout_host.second, layout_device.second, atol, rtol)
report("gpu higher-order dense transposed gradient", layout_host.dense and layout_device.dense and layout_values)

// Third order through repeated backward(track = true): d/dx x^4 = 4x^3,
// 12x^2, 24x, with every gradient on the device.
tensor<real32> quartic_source = place(pattern(6, 5), args.device)
tensor<real32> quartic = quartic_source.track()
total(quartic * quartic * quartic * quartic).backward(&quartic, track = true)
tensor<real32> d1 = quartic.grad
quartic.clear_grad()
total(d1).backward(&quartic, track = true)
tensor<real32> d2 = quartic.grad
quartic.clear_grad()
total(d2).backward(&quartic)
tensor<real32> d3 = quartic.grad
tensor<real32> q = quartic_source.cpu()
bool quartic_device = d1.device() == args.device and d2.device() == args.device and d3.device() == args.device
bool quartic_tracked = d1.is_tracked() and d2.is_tracked() and not d3.is_tracked()
report("gpu higher-order device placement", quartic_device and quartic_tracked)
bool quartic_values = close(q * q * q * real32(4), d1, atol, rtol) and close(q * q * real32(12), d2, atol, rtol) and close(q * real32(24), d3, atol, rtol)
report("gpu higher-order third derivative", quartic_values)
QUI
higher_order_output="$("$QUIDRA" "$TMP/gpu-higher-order.qui" --device 0 --bitwise true)"
higher_order_expected_count="$(grep -c '^report("' "$TMP/gpu-higher-order.qui")"
if [[ "$(grep -c ' true$' <<<"$higher_order_output" || true)" -ne "$higher_order_expected_count" ]] ||
   grep -Fq ' false' <<<"$higher_order_output"; then
    echo "unexpected fake-GPU higher-order autograd result:" >&2
    printf '%s\n' "$higher_order_output" >&2
    exit 1
fi

# Autograd regressions on gpu(n), in one program.
# Tracked negation is recorded in the graph (first order and
# backward(track = true)) instead of silently cutting it.
# The higher-order seed stays exactly one for a non-finite loss.
cat > "$TMP/gpu-autograd-defects.qui" <<'QUI'
autograd.Target negation_target = autograd.target()
tensor<real32> negation_x = tensor.ones<real32>([], gpu = 0).track(&negation_target)
tensor<real32> negated = -negation_x
print(negated.is_tracked())
print(NL)
(negation_x * negated).backward(&negation_target)
print(negation_target.gradient<real32>().cpu().item())
print(NL)
tensor<real32> cube = (tensor.ones<real32>([], gpu = 0) * real32(2)).track()
(-(cube * cube * cube)).backward(&cube, track = true)
tensor<real32> cube_first = cube.grad
print(cube_first.device() == 0)
print(NL)
print(cube_first.untrack().cpu().item())
print(NL)
cube.clear_grad()
(-cube_first).backward(&cube)
print(cube.grad.cpu().item())
print(NL)
// The higher-order seed is exactly one even when the loss is not
// finite (it used to be loss * 0 + 1 = NaN), and a leaf that the loss
// reaches only through the seed still receives its zero second derivative.
autograd.Target seed_target = autograd.target()
tensor<real32> seed_x = (tensor.ones<real32>([], gpu = 0) * real32(2)).track(&seed_target)
tensor<real32> huge = tensor.ones<real32>([], gpu = 0) * real32(300000000000000000000000000000000000000.0)
tensor<real32> overflow = seed_x * huge
print(overflow.untrack().cpu().item())
print(NL)
overflow.backward(&seed_target, track = true)
print(seed_target.gradient<real32>().untrack().cpu().item())
print(NL)
tensor<real32> linear = (tensor.ones<real32>([], gpu = 0) * real32(5)).track()
(linear * real32(3)).backward(&linear, track = true)
tensor<real32> slope = linear.grad
linear.clear_grad()
slope.backward(&linear)
print(linear.grad.cpu().item())
print(NL)
QUI
gpu_defects_output="$("$QUIDRA" "$TMP/gpu-autograd-defects.qui")"
gpu_defects_expected="$(printf 'true\n-2.0\ntrue\n-12.0\n12.0\ninf\n3.0000000054977558e+38\n0.0')"
if [[ "$gpu_defects_output" != "$gpu_defects_expected" ]]; then
    echo "unexpected fake-GPU autograd defect regression output:" >&2
    printf '%s\n' "$gpu_defects_output" >&2
    echo "expected:" >&2
    printf '%s\n' "$gpu_defects_expected" >&2
    exit 1
fi

echo "fake GPU placement contracts: ok"