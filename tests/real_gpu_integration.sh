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
QUI

output="$("$QUIDRA" run "$TMP/real-gpu.qui")"
expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$output" != "$expected" ]]; then
    echo "real GPU numerical equivalence failed on gpu($GPU_INDEX)" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

echo "real GPU integration: ok on gpu($GPU_INDEX)"
