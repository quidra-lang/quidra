#!/usr/bin/env bash
set -euo pipefail

QUIDRA="${1:-}"
if [[ -z "$QUIDRA" ]]; then
    echo "usage: $0 /path/to/quidra" >&2
    exit 2
fi

GPU_INDEX="${QUIDRA_REAL_GPU_INDEX:-0}"
REQUIRE_REAL="${QUIDRA_REQUIRE_REAL_GPU:-0}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

set +e
gpu_info="$("$QUIDRA" gpu 2>&1)"
gpu_status=$?
set -e

skip_or_fail() {
    local reason="$1"
    if [[ "$REQUIRE_REAL" == "1" ]]; then
        echo "real GPU integration required but unavailable: $reason" >&2
        printf '%s\n' "$gpu_info" >&2
        exit 1
    fi
    echo "real GPU integration: skipped ($reason)"
    exit 0
}

if [[ $gpu_status -ne 0 ]]; then
    skip_or_fail "quidra gpu failed"
fi
if grep -Fq "backend: TEST" <<<"$gpu_info"; then
    skip_or_fail "test-only fake GPU backend is active"
fi
if ! grep -Fq "GPU $GPU_INDEX" <<<"$gpu_info"; then
    skip_or_fail "gpu($GPU_INDEX) is not present"
fi

cat > "$TMP/real-gpu.qui" <<QUI
tensor<float32> cpu_a = tensor.ones<float32>([4])
tensor<float32> cpu_b = tensor.ones<float32>([4]) * 3.0
tensor<float32> gpu_a = cpu_a.gpu($GPU_INDEX)
tensor<float32> gpu_b = cpu_b.gpu($GPU_INDEX)

tensor<float32> cpu_elementwise = (cpu_a + cpu_b) * 2.0
tensor<float32> gpu_elementwise = ((gpu_a + gpu_b) * 2.0).cpu()
print(cpu_elementwise[2].item() == gpu_elementwise[2].item())
print(stats.sum(cpu_a) == stats.sum(gpu_a))
print(stats.mean(cpu_b) == stats.mean(gpu_b))

tensor<float32> cpu_left = tensor.ones<float32>([2, 3])
tensor<float32> cpu_right = tensor.ones<float32>([3, 2]) * 2.0
tensor<float32> gpu_left = cpu_left.gpu($GPU_INDEX)
tensor<float32> gpu_right = cpu_right.gpu($GPU_INDEX)

tensor<float32> cpu_mm = linear.matmul(cpu_left, cpu_right)
tensor<float32> gpu_mm = linear.matmul(gpu_left, gpu_right).cpu()
print(cpu_mm[1, 1].item() == gpu_mm[1, 1].item())

tensor<float32> cpu_v = tensor.ones<float32>([3])
tensor<float32> gpu_v = cpu_v.gpu($GPU_INDEX)
tensor<float32> cpu_mv = linear.matmul(cpu_left, cpu_v)
tensor<float32> gpu_mv = linear.matmul(gpu_left, gpu_v).cpu()
print(cpu_mv[1].item() == gpu_mv[1].item())

tensor<float32> cpu_vm = linear.matmul(cpu_v, cpu_right)
tensor<float32> gpu_vm = linear.matmul(gpu_v, gpu_right).cpu()
print(cpu_vm[1].item() == gpu_vm[1].item())

print(linear.dot(cpu_v, cpu_v) == linear.dot(gpu_v, gpu_v))

tensor<int32> cpu_i = tensor.ones<int32>([4]) * int32(7)
tensor<int32> gpu_i = cpu_i.gpu($GPU_INDEX)
tensor<int32> gpu_i_result = (gpu_i + int32(2)).cpu()
print(gpu_i_result[3].item() == int32(9))

tensor<float32> casted = float32(gpu_i)
print(casted.cpu()[0].item() == float32(7))

tensor<float32> view_source = tensor.ones<float32>([2, 3], gpu = $GPU_INDEX)
tensor<float32> view = view_source[0:2, 1:3]
tensor<float32> dense = view.contiguous().cpu()
print(dense.shape()[0] == 2 and dense.shape()[1] == 2)
print(dense[1, 1].item() == float32(1))

tensor<int8> ri8 = tensor.ones<int8>([2], gpu = $GPU_INDEX) + int8(2)
tensor<int16> ri16 = tensor.ones<int16>([2], gpu = $GPU_INDEX) * int16(3)
tensor<int32> ri32 = tensor.ones<int32>([2], gpu = $GPU_INDEX) - int32(4)
tensor<int> ri64 = tensor.ones<int>([2], gpu = $GPU_INDEX) + 5
tensor<uint8> ru8 = tensor.ones<uint8>([2], gpu = $GPU_INDEX) + uint8(6)
tensor<uint16> ru16 = tensor.ones<uint16>([2], gpu = $GPU_INDEX) * uint16(7)
tensor<uint32> ru32 = tensor.ones<uint32>([2], gpu = $GPU_INDEX) + uint32(8)
tensor<uint64> ru64 = tensor.ones<uint64>([2], gpu = $GPU_INDEX) + uint64(9)
print(ri8.cpu()[0].item() == int8(3))
print(ri16.cpu()[0].item() == int16(3))
print(ri32.cpu()[0].item() == int32(-3))
print(ri64.cpu()[0].item() == 6)
print(ru8.cpu()[0].item() == uint8(7))
print(ru16.cpu()[0].item() == uint16(7))
print(ru32.cpu()[0].item() == uint32(9))
print(ru64.cpu()[0].item() == uint64(10))

tensor<uint16> rcast = uint16(tensor.ones<int8>([2], gpu = $GPU_INDEX))
print(rcast.cpu()[1].item() == uint16(1))
tensor<int16> rdot_a = tensor.ones<int16>([3], gpu = $GPU_INDEX)
tensor<int16> rdot_b = tensor.ones<int16>([3], gpu = $GPU_INDEX)
print(linear.dot(rdot_a, rdot_b) == int16(3))
tensor<int32> rmm_a = tensor.ones<int32>([2, 2], gpu = $GPU_INDEX)
tensor<int32> rmm_b = tensor.ones<int32>([2, 2], gpu = $GPU_INDEX)
tensor<int32> rmm_c = linear.matmul(rmm_a, rmm_b).cpu()
print(rmm_c[1, 1].item() == int32(2))
print(stats.sum(ru32) == uint32(18))
print(stats.min(ri32) == int32(-3))
print(stats.max(ru64) == uint64(10))
print(stats.mean(ri16) == 3.0)

print(stats.min(cpu_b) == stats.min(gpu_b))
print(stats.max(cpu_b) == stats.max(gpu_b))
tensor<float32> negated = (-gpu_b).cpu()
print(negated[0].item() == float32(-3))
tensor<float32> scalar_add = (2.0 + gpu_a).cpu()
tensor<float32> scalar_sub_right = (gpu_b - 1.0).cpu()
tensor<float32> scalar_sub_left = (10.0 - gpu_b).cpu()
tensor<float32> scalar_div_right = (gpu_b / 3.0).cpu()
tensor<float32> scalar_div_left = (12.0 / gpu_b).cpu()
print(scalar_add[0].item() == float32(3))
print(scalar_sub_right[0].item() == float32(2))
print(scalar_sub_left[0].item() == float32(7))
print(scalar_div_right[0].item() == float32(1))
print(scalar_div_left[0].item() == float32(4))

tensor<float32><3, 2> transposed = gpu_left.transpose(0, 1)
print(transposed.shape()[0] == 3 and transposed.shape()[1] == 2)
print(transposed[2, 1].item() == float32(1))
QUI

output="$("$QUIDRA" run "$TMP/real-gpu.qui")"
expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$output" != "$expected" ]]; then
    echo "real GPU numerical equivalence failed on gpu($GPU_INDEX)" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

cat > "$TMP/integer-overflow.qui" <<QUI
tensor<int8> value = tensor.ones<int8>([1], gpu = $GPU_INDEX) * int8(127)
tensor<int8> invalid = value + int8(1)
print(invalid[0].item())
QUI
set +e
"$QUIDRA" run "$TMP/integer-overflow.qui" >"$TMP/integer-overflow.out" 2>"$TMP/integer-overflow.err"
overflow_status=$?
set -e
if [[ $overflow_status -ne 101 ]]; then
    echo "real GPU integer overflow should fail with status 101, got $overflow_status" >&2
    cat "$TMP/integer-overflow.out" >&2 || true
    cat "$TMP/integer-overflow.err" >&2 || true
    exit 1
fi
if ! grep -Fq "tensor integer arithmetic overflow" "$TMP/integer-overflow.err"; then
    echo "missing real GPU integer overflow diagnostic" >&2
    cat "$TMP/integer-overflow.err" >&2
    exit 1
fi

cat > "$TMP/integer-div-zero.qui" <<QUI
tensor<int32> value = tensor.ones<int32>([1], gpu = $GPU_INDEX)
tensor<int32> invalid = value / int32(0)
print(invalid[0].item())
QUI
set +e
"$QUIDRA" run "$TMP/integer-div-zero.qui" >"$TMP/integer-div-zero.out" 2>"$TMP/integer-div-zero.err"
divzero_status=$?
set -e
if [[ $divzero_status -ne 101 ]]; then
    echo "real GPU integer division by zero should fail with status 101, got $divzero_status" >&2
    cat "$TMP/integer-div-zero.out" >&2 || true
    cat "$TMP/integer-div-zero.err" >&2 || true
    exit 1
fi
if ! grep -Fq "invalid tensor division/remainder or integer overflow" "$TMP/integer-div-zero.err"; then
    echo "missing real GPU division-by-zero diagnostic" >&2
    cat "$TMP/integer-div-zero.err" >&2
    exit 1
fi

if grep -Fq "backend: Metal" <<<"$gpu_info"; then
    cat > "$TMP/metal-float64.qui" <<QUI
tensor<float> value = tensor.ones<float>([2], gpu = $GPU_INDEX)
tensor<float> invalid = value + value
print(invalid[0].item())
QUI
    set +e
    "$QUIDRA" run "$TMP/metal-float64.qui" >"$TMP/metal-float64.out" 2>"$TMP/metal-float64.err"
    float64_status=$?
    set -e
    if [[ $float64_status -ne 101 ]]; then
        echo "Metal float64 arithmetic should fail explicitly, got status $float64_status" >&2
        cat "$TMP/metal-float64.out" >&2 || true
        cat "$TMP/metal-float64.err" >&2 || true
        exit 1
    fi
    if ! grep -Fq "not supported on Metal" "$TMP/metal-float64.err"; then
        echo "missing explicit Metal float64 unsupported diagnostic" >&2
        cat "$TMP/metal-float64.err" >&2
        exit 1
    fi
else
    cat > "$TMP/float64-real-gpu.qui" <<QUI
tensor<float> a = tensor.ones<float>([2], gpu = $GPU_INDEX)
tensor<float> b = tensor.ones<float>([2], gpu = $GPU_INDEX) * 3.0
tensor<float> c = (a + b) / 2.0
print(c.cpu()[0].item() == 2.0)
print(stats.sum(b) == 6.0)
tensor<float> left = tensor.ones<float>([2, 2], gpu = $GPU_INDEX)
tensor<float> right = tensor.ones<float>([2, 2], gpu = $GPU_INDEX)
tensor<float> product = linear.matmul(left, right).cpu()
print(product[1, 1].item() == 2.0)
QUI
    float64_output="$("$QUIDRA" run "$TMP/float64-real-gpu.qui")"
    float64_expected="$(printf 'true\ntrue\ntrue')"
    if [[ "$float64_output" != "$float64_expected" ]]; then
        echo "real GPU float64 equivalence failed on gpu($GPU_INDEX)" >&2
        printf '%s\n' "$float64_output" >&2
        exit 1
    fi
fi

echo "real GPU integration: ok on gpu($GPU_INDEX)"
