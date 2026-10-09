#!/usr/bin/env bash
set -euo pipefail

QUIDRA="${1:-}"
if [[ -z "$QUIDRA" ]]; then
    echo "usage: $0 /path/to/quidra" >&2
    exit 2
fi

GPU_INDEX="${QUIDRA_REAL_GPU_INDEX:-0}"
REQUIRE_REAL="${QUIDRA_REQUIRE_REAL_GPU:-0}"
REQUIRE_BACKEND="${QUIDRA_REQUIRE_GPU_BACKEND:-}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

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
if [[ -n "$REQUIRE_BACKEND" ]]; then
    gpu_block="$(awk -v target="GPU $GPU_INDEX" '
        $0 == target { found = 1; print; next }
        found && /^GPU [0-9]+$/ { exit }
        found { print }
    ' <<<"$gpu_info")"
    if ! grep -Fq "backend: $REQUIRE_BACKEND" <<<"$gpu_block"; then
        skip_or_fail "gpu($GPU_INDEX) is not backend $REQUIRE_BACKEND"
    fi
fi

cat > "$TMP/real-gpu.qui" <<QUI
tensor<real32> cpu_a = tensor.ones<real32>([4])
tensor<real32> cpu_b = tensor.ones<real32>([4]) * 3.0
tensor<real32> gpu_a = cpu_a.gpu($GPU_INDEX)
tensor<real32> gpu_b = cpu_b.gpu($GPU_INDEX)

time.Instant gpu_timer_start = time.now(sync = true)
tensor<real32> gpu_timer_value = (gpu_a + gpu_b) * 2.0
time.Duration gpu_timer_elapsed = time.since(gpu_timer_start, sync = true)
print(gpu_timer_elapsed.seconds() >= 0.0)
print(NL)

tensor<real32> reuse_result = tensor.zeros<real32>([4096], gpu = $GPU_INDEX)
for i in range(64)
    real32 scale = real32(i + 1)
    tensor<real32> temporary = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * scale
    reuse_result = temporary + 1.0
gpu.sync($GPU_INDEX)
print(reuse_result[0].item() == real32(65))
print(NL)

tensor<real32> cpu_elementwise = (cpu_a + cpu_b) * 2.0
tensor<real32> gpu_elementwise = ((gpu_a + gpu_b) * 2.0).cpu()
print(cpu_elementwise[2].item() == gpu_elementwise[2].item())
print(NL)
print((gpu_a == gpu_a).all())
print(NL)
print((gpu_a != gpu_b).any())
print(NL)
print((gpu_a < gpu_b).all())
print(NL)
print((gpu_a <= gpu_b).all())
print(NL)
print((gpu_b > gpu_a).all())
print(NL)
print((gpu_b >= gpu_a).all())
print(NL)

tensor<real32> compare_mixed = tensor.ones<real32>([4], gpu = $GPU_INDEX)
compare_mixed[3] = real32(2)
print((gpu_a != compare_mixed).any())
print(NL)

tensor<real32> compare_view_source = tensor.ones<real32>([2, 3], gpu = $GPU_INDEX)
tensor<real32> compare_view_a = compare_view_source[0:2, 1:3]
tensor<real32> compare_view_b = compare_view_source[0:2, 1:3]
print((compare_view_a == compare_view_b).all())
print(NL)

tensor<real32> gpu_left = tensor.ones<real32>([2, 3], gpu = $GPU_INDEX)

tensor<int32> cpu_i = tensor.ones<int32>([4]) * int32(7)
tensor<int32> gpu_i = cpu_i.gpu($GPU_INDEX)
tensor<int32> gpu_i_result = (gpu_i + int32(2)).cpu()
print(gpu_i_result[3].item() == int32(9))
print(NL)
tensor<int32> gpu_i_remainder = (gpu_i % int32(4)).cpu()
print(gpu_i_remainder[3].item() == int32(3))
print(NL)

tensor<real32> casted = real32(gpu_i)
print(casted.cpu()[0].item() == real32(7))
print(NL)

tensor<real32> view_source = tensor.ones<real32>([2, 3], gpu = $GPU_INDEX)
tensor<real32> view = view_source[0:2, 1:3]
tensor<real32> dense = view.contiguous().cpu()
print(dense.shape()[0] == 2 and dense.shape()[1] == 2)
print(NL)
print(dense[1, 1].item() == real32(1))
print(NL)

tensor<int8> ri8 = tensor.ones<int8>([2], gpu = $GPU_INDEX) + int8(2)
tensor<int16> ri16 = tensor.ones<int16>([2], gpu = $GPU_INDEX) * int16(3)
tensor<int32> ri32 = tensor.ones<int32>([2], gpu = $GPU_INDEX) - int32(4)
tensor<int64> ri64 = tensor.ones<int64>([2], gpu = $GPU_INDEX) + 5
tensor<nat8> ru8 = tensor.ones<nat8>([2], gpu = $GPU_INDEX) + nat8(6)
tensor<nat16> ru16 = tensor.ones<nat16>([2], gpu = $GPU_INDEX) * nat16(7)
tensor<nat32> ru32 = tensor.ones<nat32>([2], gpu = $GPU_INDEX) + nat32(8)
tensor<nat64> ru64 = tensor.ones<nat64>([2], gpu = $GPU_INDEX) + nat64(9)
print(ri8.cpu()[0].item() == int8(3))
print(NL)
print(ri16.cpu()[0].item() == int16(3))
print(NL)
print(ri32.cpu()[0].item() == int32(-3))
print(NL)
print(ri64.cpu()[0].item() == 6)
print(NL)
print(ru8.cpu()[0].item() == nat8(7))
print(NL)
print(ru16.cpu()[0].item() == nat16(7))
print(NL)
print(ru32.cpu()[0].item() == nat32(9))
print(NL)
print(ru64.cpu()[0].item() == nat64(10))
print(NL)

tensor<nat16> rcast = nat16(tensor.ones<int8>([2], gpu = $GPU_INDEX))
print(rcast.cpu()[1].item() == nat16(1))
print(NL)

tensor<int32> scatter_values = tensor.zeros<int32>([3], gpu = $GPU_INDEX)
scatter_values[0] = int32(1)
scatter_values[1] = int32(2)
scatter_values[2] = int32(3)
tensor<int32> scatter_result = scatter_values.scatter([0, 0, 2], [4]).cpu()
print(scatter_result[0].item() == int32(3))
print(NL)
print(scatter_result[1].item() == int32(0))
print(NL)
print(scatter_result[2].item() == int32(3))
print(NL)
print(scatter_result[3].item() == int32(0))
print(NL)

tensor<real32> negated = (-gpu_b).cpu()
print(negated[0].item() == real32(-3))
print(NL)
tensor<real32> scalar_add = (2.0 + gpu_a).cpu()
tensor<real32> scalar_sub_right = (gpu_b - 1.0).cpu()
tensor<real32> scalar_sub_left = (10.0 - gpu_b).cpu()
tensor<real32> scalar_div_right = (gpu_b / 3.0).cpu()
tensor<real32> scalar_div_left = (12.0 / gpu_b).cpu()
print(scalar_add[0].item() == real32(3))
print(NL)
print(scalar_sub_right[0].item() == real32(2))
print(NL)
print(scalar_sub_left[0].item() == real32(7))
print(NL)
print(scalar_div_right[0].item() == real32(1))
print(NL)
print(scalar_div_left[0].item() == real32(4))
print(NL)

tensor<real32><3, 2> transposed = gpu_left.transpose(0, 1)
print(transposed.shape()[0] == 3 and transposed.shape()[1] == 2)
print(NL)
print(transposed[2, 1].item() == real32(1))
print(NL)

tensor<int32> cow_original = tensor.ones<int32>([2], gpu = $GPU_INDEX)
tensor<int32> cow_copy = cow_original
cow_copy[0] = int32(9)
print(cow_original[0].item() == int32(1))
print(NL)
print(cow_copy[0].item() == int32(9))
print(NL)

tensor<int32> direct = tensor<int32>([2], gpu = $GPU_INDEX)
direct[0] = int32(4)
direct[1] = int32(5)
print(direct[0].item() == int32(4) and direct[1].item() == int32(5))
print(NL)
QUI

output="$("$QUIDRA" run "$TMP/real-gpu.qui")"
expected="$(printf 'true\n%.0s' {1..40})"
if [[ "$output" != "$expected" ]]; then
    echo "real GPU numerical equivalence failed on gpu($GPU_INDEX)" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

cat > "$TMP/async-boundaries.qui" <<QUI
tensor<real32> source = tensor.ones<real32>([1024], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
time.Instant start = time.now()
tensor<real32> queued = (source + 2.0) * 3.0
time.Duration elapsed = time.since(start)
print(elapsed.seconds() >= 0.0)
print(NL)
print(queued.cpu()[0].item() == real32(9))
print(NL)
QUI
async_boundary_output="$("$QUIDRA" run "$TMP/async-boundaries.qui")"
async_boundary_expected="$(printf 'true\ntrue')"
if [[ "$async_boundary_output" != "$async_boundary_expected" ]]; then
    echo "real GPU synchronization/timing boundary failed on gpu($GPU_INDEX)" >&2
    printf '%s\n' "$async_boundary_output" >&2
    exit 1
fi

cat > "$TMP/async-lifetime.qui" <<QUI
tensor<real32> queued_from_temporary(int gpu_index)
    tensor<real32> source = tensor.ones<real32>([4194304], gpu = nat(gpu_index))
    return source * 3.0

tensor<real32> queued_lifetime = queued_from_temporary($GPU_INDEX)
tensor<real32> reuse_pressure = tensor.zeros<real32>([4194304], gpu = $GPU_INDEX)
print(queued_lifetime.cpu()[4194303].item() == real32(3))
print(NL)
print(reuse_pressure.cpu()[0].item() == real32(0))
print(NL)
QUI
async_lifetime_output="$("$QUIDRA" run "$TMP/async-lifetime.qui")"
async_lifetime_expected="$(printf 'true\ntrue')"
if [[ "$async_lifetime_output" != "$async_lifetime_expected" ]]; then
    echo "real GPU asynchronous buffer lifetime failed on gpu($GPU_INDEX)" >&2
    printf '%s\n' "$async_lifetime_output" >&2
    exit 1
fi

# Math owns mathematical tensor semantics and their GPU/autograd coverage.
# Core's real-GPU suite is limited to tensor/device/autograd substrate behavior.



cat > "$TMP/integer-overflow.qui" <<QUI
tensor<int8> value = tensor.ones<int8>([1], gpu = $GPU_INDEX) * int8(127)
tensor<int8> invalid = value + int8(1)
print(invalid[0].item())
print(NL)
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
if grep -Fq "backend: Metal" <<<"$gpu_info" &&
   ! grep -Fq "(deferred GPU check from $TMP/integer-overflow.qui:2:" "$TMP/integer-overflow.err"; then
    echo "deferred GPU check does not name the statement that queued it" >&2
    cat "$TMP/integer-overflow.err" >&2
    exit 1
fi

# A deferred check that fails at program exit (GPU_ASYNC) keeps the output
# the program wrote before it: the exit guard flushes it ahead of the report.
cat > "$TMP/exit-check-keeps-output.qui" <<QUI
tensor<int8> value = tensor.ones<int8>([1], gpu = $GPU_INDEX) * int8(127)
print("before" + NL)
tensor<int8> invalid = value + int8(1)
print("after" + NL)
QUI
set +e
"$QUIDRA" run "$TMP/exit-check-keeps-output.qui" >"$TMP/exit-check-keeps-output.out" 2>"$TMP/exit-check-keeps-output.err"
exit_check_output_status=$?
set -e
if [[ $exit_check_output_status -ne 101 ]] ||
   [[ "$(cat "$TMP/exit-check-keeps-output.out")" != "$(printf 'before\nafter')" ]] ||
   ! grep -Fxq "Quidra runtime error[GPU_ASYNC] at $TMP/exit-check-keeps-output.qui" "$TMP/exit-check-keeps-output.err" ||
   ! grep -Fq "| tensor integer arithmetic overflow" "$TMP/exit-check-keeps-output.err"; then
    echo "a deferred check failing at exit lost the program's output" >&2
    cat "$TMP/exit-check-keeps-output.out" "$TMP/exit-check-keeps-output.err" >&2
    exit 1
fi

cat > "$TMP/scatter-overflow.qui" <<QUI
tensor<int8> values = tensor.ones<int8>([2], gpu = $GPU_INDEX) * int8(127)
tensor<int8> invalid = values.scatter([0, 0], [1])
print(invalid.cpu()[0].item())
print(NL)
QUI
set +e
"$QUIDRA" run "$TMP/scatter-overflow.qui" >"$TMP/scatter-overflow.out" 2>"$TMP/scatter-overflow.err"
scatter_overflow_status=$?
set -e
if [[ $scatter_overflow_status -ne 101 ]]; then
    echo "real GPU integer scatter overflow should fail with status 101, got $scatter_overflow_status" >&2
    cat "$TMP/scatter-overflow.out" >&2 || true
    cat "$TMP/scatter-overflow.err" >&2 || true
    exit 1
fi
if ! grep -Fq "tensor.scatter integer arithmetic overflow" "$TMP/scatter-overflow.err"; then
    echo "missing real GPU scatter overflow diagnostic" >&2
    cat "$TMP/scatter-overflow.err" >&2
    exit 1
fi

cat > "$TMP/deferred-overflow-exit.qui" <<QUI
tensor<int8> value = tensor.ones<int8>([1], gpu = $GPU_INDEX) * int8(127)
tensor<int8> invalid = value + int8(1)
QUI
set +e
"$QUIDRA" run "$TMP/deferred-overflow-exit.qui" >"$TMP/deferred-overflow-exit.out" 2>"$TMP/deferred-overflow-exit.err"
deferred_overflow_status=$?
set -e
if [[ $deferred_overflow_status -ne 101 ]]; then
    echo "unobserved checked GPU failure must surface at process shutdown, got $deferred_overflow_status" >&2
    cat "$TMP/deferred-overflow-exit.out" >&2 || true
    cat "$TMP/deferred-overflow-exit.err" >&2 || true
    exit 1
fi
if ! grep -Fq "tensor integer arithmetic overflow" "$TMP/deferred-overflow-exit.err"; then
    echo "missing deferred GPU shutdown diagnostic" >&2
    cat "$TMP/deferred-overflow-exit.err" >&2
    exit 1
fi

cat > "$TMP/integer-div-zero.qui" <<QUI
tensor<int32> value = tensor.ones<int32>([1], gpu = $GPU_INDEX)
tensor<int32> invalid = value / int32(0)
print(invalid[0].item())
print(NL)
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

cat > "$TMP/unsigned-underflow.qui" <<QUI
tensor<nat8> value = tensor.zeros<nat8>([1], gpu = $GPU_INDEX)
tensor<nat8> invalid = value - nat8(1)
print(invalid[0].item())
print(NL)
QUI
set +e
"$QUIDRA" run "$TMP/unsigned-underflow.qui" >"$TMP/unsigned-underflow.out" 2>"$TMP/unsigned-underflow.err"
underflow_status=$?
set -e
if [[ $underflow_status -ne 101 ]]; then
    echo "real GPU unsigned underflow should fail with status 101, got $underflow_status" >&2
    cat "$TMP/unsigned-underflow.out" >&2 || true
    cat "$TMP/unsigned-underflow.err" >&2 || true
    exit 1
fi
if ! grep -Fq "tensor integer arithmetic overflow" "$TMP/unsigned-underflow.err"; then
    echo "missing real GPU unsigned-underflow diagnostic" >&2
    cat "$TMP/unsigned-underflow.err" >&2
    exit 1
fi

cat > "$TMP/integer-min-div-negative-one.qui" <<QUI
tensor<int8> value = tensor.ones<int8>([1], gpu = $GPU_INDEX) * int8(-128)
tensor<int8> invalid = value / int8(-1)
print(invalid[0].item())
print(NL)
QUI
set +e
"$QUIDRA" run "$TMP/integer-min-div-negative-one.qui" >"$TMP/integer-min-div-negative-one.out" 2>"$TMP/integer-min-div-negative-one.err"
min_div_status=$?
set -e
if [[ $min_div_status -ne 101 ]]; then
    echo "real GPU signed min / -1 should fail with status 101, got $min_div_status" >&2
    cat "$TMP/integer-min-div-negative-one.out" >&2 || true
    cat "$TMP/integer-min-div-negative-one.err" >&2 || true
    exit 1
fi
if ! grep -Fq "invalid tensor division/remainder or integer overflow" "$TMP/integer-min-div-negative-one.err"; then
    echo "missing real GPU signed min / -1 diagnostic" >&2
    cat "$TMP/integer-min-div-negative-one.err" >&2
    exit 1
fi

cat > "$TMP/integer-min-remainder-negative-one.qui" <<QUI
tensor<int8> value = tensor.ones<int8>([1], gpu = $GPU_INDEX) * int8(-128)
tensor<int8> remainder = value % int8(-1)
print(remainder[0].item())
print(NL)
QUI
if [[ "$("$QUIDRA" run "$TMP/integer-min-remainder-negative-one.qui")" != "0" ]]; then
    echo "real GPU signed min % -1 must match CPU semantics" >&2
    exit 1
fi

cat > "$TMP/integer-cast-range.qui" <<QUI
tensor<int16> source = tensor.ones<int16>([1], gpu = $GPU_INDEX) * int16(300)
tensor<int8> invalid = int8(source)
print(invalid[0].item())
print(NL)
QUI
set +e
"$QUIDRA" run "$TMP/integer-cast-range.qui" >"$TMP/integer-cast-range.out" 2>"$TMP/integer-cast-range.err"
cast_status=$?
set -e
if [[ $cast_status -ne 101 ]]; then
    echo "real GPU out-of-range cast should fail with status 101, got $cast_status" >&2
    cat "$TMP/integer-cast-range.out" >&2 || true
    cat "$TMP/integer-cast-range.err" >&2 || true
    exit 1
fi
if ! grep -Fq "numeric conversion out of range: tensor element cannot be represented as int8" "$TMP/integer-cast-range.err"; then
    echo "missing real GPU cast-range diagnostic" >&2
    cat "$TMP/integer-cast-range.err" >&2
    exit 1
fi

# backward(track = true) on the real GPU against the CPU (per-quantity
# atol/rtol, explained in the program: GPU kernels may round differently from
# the CPU) and against central differences, for binary, scalar, view and
# gather gradients, a gradient stored dense after a transpose, and a third
# derivative.
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
higher_order_output="$("$QUIDRA" "$TMP/gpu-higher-order.qui" --device "$GPU_INDEX")"
higher_order_expected_count="$(grep -c '^report("' "$TMP/gpu-higher-order.qui")"
if [[ "$(grep -c ' true$' <<<"$higher_order_output" || true)" -ne "$higher_order_expected_count" ]] ||
   grep -Fq ' false' <<<"$higher_order_output"; then
    echo "real GPU higher-order autograd failed on gpu($GPU_INDEX)" >&2
    printf '%s\n' "$higher_order_output" >&2
    exit 1
fi

# Autograd regressions on the real GPU, in one program.
# Tracked negation is recorded in the graph (first order and
# backward(track = true)) instead of silently cutting it.
# The higher-order seed stays exactly one for a non-finite loss.
cat > "$TMP/gpu-autograd-defects.qui" <<QUI
autograd.Target negation_target = autograd.target()
tensor<real32> negation_x = tensor.ones<real32>([], gpu = $GPU_INDEX).track(&negation_target)
tensor<real32> negated = -negation_x
print(negated.is_tracked())
print(NL)
(negation_x * negated).backward(&negation_target)
print(negation_target.gradient<real32>().cpu().item())
print(NL)
tensor<real32> cube = (tensor.ones<real32>([], gpu = $GPU_INDEX) * real32(2)).track()
(-(cube * cube * cube)).backward(&cube, track = true)
tensor<real32> cube_first = cube.grad
print(cube_first.device() == $GPU_INDEX)
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
tensor<real32> seed_x = (tensor.ones<real32>([], gpu = $GPU_INDEX) * real32(2)).track(&seed_target)
tensor<real32> huge = tensor.ones<real32>([], gpu = $GPU_INDEX) * real32(300000000000000000000000000000000000000.0)
tensor<real32> overflow = seed_x * huge
print(overflow.untrack().cpu().item())
print(NL)
overflow.backward(&seed_target, track = true)
print(seed_target.gradient<real32>().untrack().cpu().item())
print(NL)
tensor<real32> linear = (tensor.ones<real32>([], gpu = $GPU_INDEX) * real32(5)).track()
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
    echo "real GPU autograd defect regressions failed on gpu($GPU_INDEX):" >&2
    printf '%s\n' "$gpu_defects_output" >&2
    echo "expected:" >&2
    printf '%s\n' "$gpu_defects_expected" >&2
    exit 1
fi

if grep -Fq "backend: Metal" <<<"$gpu_info"; then
    cat > "$TMP/metal-float64.qui" <<QUI
tensor<real64> value = tensor.ones<real64>([2], gpu = $GPU_INDEX)
tensor<real64> invalid = value + value
print(invalid[0].item())
print(NL)
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
tensor<real64> a = tensor.ones<real64>([2], gpu = $GPU_INDEX)
tensor<real64> b = tensor.ones<real64>([2], gpu = $GPU_INDEX) * 3.0
tensor<real64> c = (a + b) / 2.0
print(c.cpu()[0].item() == 2.0)
print(NL)
QUI
    float64_output="$("$QUIDRA" run "$TMP/float64-real-gpu.qui")"
    float64_expected="true"
    if [[ "$float64_output" != "$float64_expected" ]]; then
        echo "real GPU float64 equivalence failed on gpu($GPU_INDEX)" >&2
        printf '%s\n' "$float64_output" >&2
        exit 1
    fi
fi

# Broadcast, transposed, stepped and scalar arithmetic and their gradients
# read strided operands on the device (QUIDRA_BROADCAST=strided). Every value
# must be bitwise the one the host-built gather-index path
# (QUIDRA_BROADCAST=gather) produces.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$QUIDRA" build "$SCRIPT_DIR/gpu_broadcast_bitwise.qui" -o "$TMP/broadcast-bitwise" >/dev/null
QUIDRA_BROADCAST=strided "$TMP/broadcast-bitwise" --device "$GPU_INDEX" \
    >"$TMP/broadcast-strided.out"
QUIDRA_BROADCAST=gather "$TMP/broadcast-bitwise" --device "$GPU_INDEX" \
    >"$TMP/broadcast-gather.out"
if ! cmp -s "$TMP/broadcast-strided.out" "$TMP/broadcast-gather.out"; then
    echo "strided broadcast differs from the gather path on gpu($GPU_INDEX)" >&2
    diff "$TMP/broadcast-gather.out" "$TMP/broadcast-strided.out" | head -20 >&2
    exit 1
fi

# Zero-extent broadcasting on the device: a singleton axis takes the
# other side's extent, including 0, in both broadcast modes, scalars
# broadcast to any rank, and shapes that are not broadcast-compatible fail
# with a defined runtime error, with untracked and tracked operands. The
# values are small integers, so the table is the CPU's text exactly.
"$QUIDRA" build "$SCRIPT_DIR/broadcast_zero_extent.qui" -o "$TMP/broadcast-zero-extent" >/dev/null
"$TMP/broadcast-zero-extent" --device -1 >"$TMP/broadcast-zero-cpu.out"
grep -Fxq "zero-one d_right [1,3]: 0.0 0.0 0.0" "$TMP/broadcast-zero-cpu.out" || {
    echo "zero-extent broadcasting: unexpected CPU table" >&2
    cat "$TMP/broadcast-zero-cpu.out" >&2
    exit 1
}
for mode in strided gather; do
    QUIDRA_BROADCAST="$mode" "$TMP/broadcast-zero-extent" --device "$GPU_INDEX" \
        >"$TMP/broadcast-zero-$mode.out"
    if ! cmp -s "$TMP/broadcast-zero-cpu.out" "$TMP/broadcast-zero-$mode.out"; then
        echo "zero-extent broadcasting ($mode) differs from the CPU on gpu($GPU_INDEX)" >&2
        diff "$TMP/broadcast-zero-cpu.out" "$TMP/broadcast-zero-$mode.out" >&2 || true
        exit 1
    fi
    for case_index in 1 2 3 4 5 6 7 8; do
        expected="tensor broadcasting requires identical ranks"
        if [[ "$case_index" -eq 1 || "$case_index" -eq 5 ]]; then
            expected="tensor shapes are not broadcast-compatible"
        fi
        for tracked in 0 1 2 3; do
            set +e
            QUIDRA_BROADCAST="$mode" "$TMP/broadcast-zero-extent" --device "$GPU_INDEX" \
                --case "$case_index" --tracked "$tracked" \
                >"$TMP/broadcast-zero-error.out" 2>"$TMP/broadcast-zero-error.err"
            status=$?
            set -e
            if [[ "$status" -ne 101 || -s "$TMP/broadcast-zero-error.out" ]] ||
               ! grep -Fq "$expected" "$TMP/broadcast-zero-error.err"; then
                echo "zero-extent broadcast error case $case_index tracked $tracked ($mode) gave status $status on gpu($GPU_INDEX)" >&2
                cat "$TMP/broadcast-zero-error.out" "$TMP/broadcast-zero-error.err" >&2
                exit 1
            fi
        done
    done
done

if grep -Fq "backend: Metal" <<<"$gpu_info"; then
    # Core's Metal command stream: a chain of Core operations shares one open
    # command buffer, host reads flush and wait, and the values equal those
    # of one command buffer per operation (QUIDRA_METAL_STREAM=0).
    cat > "$TMP/metal-stream.qui" <<QUI
tensor<real32> value = tensor.ones<real32>([1024], gpu = $GPU_INDEX)
for step in range(100)
    value = value * 1.0001 + 0.5
print(value[7].item())
print(NL)
tensor<real32> other = value / 3.0
print(other.cpu()[1023].item())
print(NL)
QUI
    stream_on="$(QUIDRA_COUNTERS="$TMP/metal-stream.jsonl" "$QUIDRA" run "$TMP/metal-stream.qui")"
    stream_off="$(QUIDRA_METAL_STREAM=0 "$QUIDRA" run "$TMP/metal-stream.qui")"
    if [[ "$stream_on" != "$stream_off" ]]; then
        echo "Metal command stream changed values: '$stream_on' vs '$stream_off'" >&2
        exit 1
    fi
    # Pooled buffers are poisoned when handed out: a buffer reused while a
    # command buffer still reads it, or an output that was not completely
    # written, would change the values.
    pool_poison="$(QUIDRA_TEST_POOL_POISON=0x7f "$QUIDRA" run "$TMP/metal-stream.qui")"
    pool_off="$(QUIDRA_METAL_POOL=0 "$QUIDRA" run "$TMP/metal-stream.qui")"
    blit_upload="$(QUIDRA_METAL_UPLOAD=blit QUIDRA_GPU_ZERO_FILL=always "$QUIDRA" run "$TMP/metal-stream.qui")"
    if [[ "$pool_poison" != "$stream_off" || "$pool_off" != "$stream_off" ||
          "$blit_upload" != "$stream_off" ]]; then
        echo "Metal pool/upload paths changed values: '$pool_poison' / '$pool_off' / '$blit_upload' vs '$stream_off'" >&2
        exit 1
    fi
    python3 - "$TMP/metal-stream.jsonl" <<'PY'
import json, sys
total = [json.loads(l) for l in open(sys.argv[1]) if '"total"' in l][0]
# 200 element-wise dispatches plus the division share command buffers (at
# least eight operations per buffer: the idle-GPU commit floor), instead of
# one buffer per operation; each host read waits once.
assert total["core_dispatches"] >= 201, total
# Temporaries released in the loop are reused once their command buffer
# completed, instead of a new Metal allocation per operation.
assert total["metal_pool_hits"] > 0, total
# Device work reuses a released buffer in queue order, so a long chain
# does not outrun the pool while its command buffers are still running.
assert total["metal_new_buffers"] <= 16, total
# Fills of idle buffers are host writes and in-flight fills are compute
# kernels: no blit encoder and no staging upload on this path.
assert total["metal_blit_encoders"] == 0, total
assert total["metal_staged_uploads"] == 0, total
operations = total["core_dispatches"] + total["fills"] + total["uploads"]
assert total["metal_command_buffers"] * 8 <= operations, total
assert total["host_waits_read"] <= 2, total
PY

    # In safe math mode without contraction (QUIDRA_METAL_SAFE_MATH=1
    # until the packages switch with it), Core's float32 division is
    # correctly rounded and equals the CPU bit for bit.
    cat > "$TMP/metal-ieee-division.qui" <<QUI
int n = 4096
tensor<real32> a = tensor<real32>([nat(n)])
tensor<real32> b = tensor<real32>([nat(n)])
real32 x = real32(0.7316)
real32 y = real32(1.913)
for i in range(n)
    x = x * real32(1.37) + real32(0.11)
    if x > real32(1000)
        x = x / real32(997)
    y = y * real32(1.21) + real32(0.37)
    if y > real32(100)
        y = y / real32(93)
    a[i] = x
    b[i] = y
tensor<real32> cpu_quotient = a / b
tensor<real32> gpu_quotient = (a.gpu($GPU_INDEX) / b.gpu($GPU_INDEX)).cpu()
int mismatches = 0
for i in range(n)
    if cpu_quotient[i].item() != gpu_quotient[i].item()
        mismatches += 1
print(mismatches)
print(NL)
QUI
    if [[ "$(QUIDRA_METAL_SAFE_MATH=1 "$QUIDRA" run "$TMP/metal-ieee-division.qui")" != "0" ]]; then
        echo "Metal float32 division in safe math mode differs from the CPU" >&2
        exit 1
    fi

    # Concurrent task.all threads share the device stream; each one's reads
    # see its own completed work.
    cat > "$TMP/metal-stream-threads.qui" <<QUI
real64 run(real32 seed)
    tensor<real32> value = tensor.ones<real32>([256], gpu = $GPU_INDEX) * seed
    for step in range(50)
        value = value + 1.0
    gpu.sync($GPU_INDEX)
    return real64(value[3].item())

real64 run1()
    return run(real32(1))
real64 run2()
    return run(real32(2))
real64 run3()
    return run(real32(3))
real64 run4()
    return run(real32(4))

fn<real64>()[] operations = [run1, run2, run3, run4]
real64[] results = task.all(operations)
print(results[0] == 51.0 and results[1] == 52.0 and results[2] == 53.0 and results[3] == 54.0)
print(NL)
QUI
    if [[ "$("$QUIDRA" run "$TMP/metal-stream-threads.qui")" != "true" ]]; then
        echo "concurrent Metal stream users saw incomplete work" >&2
        exit 1
    fi

    # Package code on Core's command stream: encodes into Core's encoder,
    # into Core's command buffer with its own encoder, and (unmigrated) into a
    # command buffer of its own committed without waiting. Each result must
    # be ordered after the Core work before it and visible to the reads after.
    mkdir -p "$TMP/packages/stream_pkg/native"
    cat > "$TMP/packages/stream_pkg/quidra.package" <<'MANIFEST'
name = stream_pkg
version = 0.1.0
native.source.macos-arm64.metal = native/stream.mm
native.source.macos-x86_64.metal = native/stream.mm
MANIFEST
    cat > "$TMP/packages/stream_pkg/main.qui" <<'QUI'
extern int32 scale_native(
    const tensor<real32> &input,
    tensor<real32> &output,
    real32 factor,
    int32 mode
) = "qtest_stream_scale"
extern int32 completions_native() = "qtest_stream_completions"
extern int32 check_native(
    const tensor<real32> &input,
    int32 deferred
) = "qtest_stream_check"
extern int32 slow_native(
    const tensor<real32> &input,
    tensor<real32> &output,
    int32 iterations,
    int32 mode
) = "qtest_stream_slow"
extern int32 slow_status_native() = "qtest_stream_slow_status"
extern int32 scope_status_native(const tensor<real32> &input) = "qtest_stream_scope_status"
extern int32 arm_late_check_native(const tensor<real32> &input) = "qtest_stream_arm_late_check"
extern int32 late_check_state_native() = "qtest_stream_late_check_state"

tensor<real32> scale(tensor<real32> input, real32 factor, int32 mode)
    tensor<real32> output = tensor<real32>(input.shape(), gpu = nat(input.device()))
    int32 status = scale_native(&input, &output, factor, mode)
    if status != int32(0)
        error("stream package scale failed with status {status}")
    return output

int32 completions()
    return completions_native()

int32 check(tensor<real32> input, int32 deferred)
    return check_native(&input, deferred)

tensor<real32> slow(tensor<real32> input, int32 iterations, int32 mode)
    tensor<real32> output = tensor<real32>(input.shape(), gpu = nat(input.device()))
    int32 status = slow_native(&input, &output, iterations, mode)
    if status != int32(0)
        error("stream package slow failed with status {status}")
    return output

int32 slow_status()
    return slow_status_native()

int32 scope_status(tensor<real32> input)
    return scope_status_native(&input)

int32 arm_late_check(tensor<real32> input)
    return arm_late_check_native(&input)

int32 late_check_state()
    return late_check_state_native()

extern int32 arm_callback_native(const tensor<real32> &input, int32 mode) = "qtest_stream_arm_callback"
extern int32 callback_state_native() = "qtest_stream_callback_state"

int32 arm_callback(tensor<real32> input, int32 mode)
    return arm_callback_native(&input, mode)

int32 callback_state()
    return callback_state_native()

extern int32 scratch_fill_native(tensor<real32> &output, real32 value) = "qtest_stream_scratch_fill"
extern int32 hooks_native(int32 which) = "qtest_stream_hooks"
extern int32 scope_queue_native(const tensor<real32> &input, int32 iterations) = "qtest_stream_scope_queue"

int32 scope_queue(tensor<real32> input, int32 iterations)
    return scope_queue_native(&input, iterations)

extern int32 device_wait_native(const tensor<real32> &input) = "qtest_stream_device_wait"
extern int32 warmup_state_native(const tensor<real32> &input, int32 which) = "qtest_stream_warmup_state"

int32 warmup_state(tensor<real32> input, int32 which)
    return warmup_state_native(&input, which)

int32 device_wait(tensor<real32> input)
    return device_wait_native(&input)

int32 hooks(int32 which)
    return hooks_native(which)

tensor<real32> scratch_fill(tensor<real32> output, real32 value)
    int32 status = scratch_fill_native(&output, value)
    if status != int32(0)
        error("stream package scratch fill failed with status {status}")
    return output

extern int32 inplace_native(tensor<real32> &t, int32 mode) = "qtest_stream_inplace"

tensor<real32> inplace(tensor<real32> t, int32 mode)
    int32 status = inplace_native(&t, mode)
    if status != int32(0)
        error("stream package inplace failed with status {status}")
    return t

extern int32 violate_native(tensor<real32> &t, int32 kind) = "qtest_stream_violate"

tensor<real32> violate(tensor<real32> t, int32 kind)
    int32 status = violate_native(&t, kind)
    if status != int32(0)
        error("stream package violate failed with status {status}")
    return t

extern int32 cpu_borrow_native(const tensor<real32> &input, tensor<real32> &host, int32 mode) = "qtest_stream_cpu_borrow"

int32 cpu_borrow(tensor<real32> input, tensor<real32> &host, int32 mode)
    return cpu_borrow_native(&input, &host, mode)

// Each leaves a package encode open (kind 0: encoder scope, 1: command-buffer
// scope, 2: hold only) and returns. The leak_* functions then ask Core for
// work in the same statement: a kernel, a fill, an upload, a same-device
// copy, or a checked cast whose status page Core clears first.
extern int32 leak_native(const tensor<real32> &t, int32 kind) = "qtest_stream_leak"

int32 leak(tensor<real32> t, int32 kind)
    return leak_native(&t, kind)

// A task holds the stream (mode 0: then asks for a refused flush and ends
// the hold; mode 1: returns with the hold open) while another task, which
// await_hold lets go once the hold is taken, waits for the stream
// (wait_report: a device wait, reported on stderr as soon as it returns).
extern int32 task_hold_native(const tensor<real32> &t, int32 mode) = "qtest_stream_task_hold"
extern int32 await_hold_native() = "qtest_stream_await_hold"
extern int32 wait_report_native(const tensor<real32> &t) = "qtest_stream_wait_report"

int32 task_hold(tensor<real32> t, int32 mode)
    return task_hold_native(&t, mode)

int32 await_hold()
    return await_hold_native()

int32 wait_report(tensor<real32> t)
    return wait_report_native(&t)

tensor<real32> leak_kernel(tensor<real32> t, int32 kind)
    return t * real32(leak_native(&t, kind) + int32(3))

tensor<real32> leak_fill(tensor<real32> t, int32 kind)
    return tensor.zeros<real32>([nat(int(leak_native(&t, kind)) + 4096)], gpu = nat(t.device()))

tensor<real32> leak_upload(tensor<real32> t, tensor<real32> host, int32 kind)
    return (host + real32(leak_native(&t, kind))).gpu(nat(t.device()))

tensor<real32> leak_transfer(tensor<real32> t, int32 kind)
    return t.gpu(nat(int(leak_native(&t, kind)) + t.device()))

tensor<real32> second(int32 status, tensor<real32> value)
    return value

tensor<real32> leak_cast(tensor<real32> t, tensor<int32> ints, int32 kind)
    return second(leak_native(&t, kind), real32(ints))

// output = 2 * input with a custom autograd backward that breaks the encode
// protocol (kind 0: hold left open, 1: encoder scope left open, 2: a flush
// in the hold, ended with qcore_metal_note_work).
extern int32 break_backward_native(const tensor<real32> &input, tensor<real32> &output, int32 kind) = "qtest_stream_break_backward"

tensor<real32> break_backward(tensor<real32> input, int32 kind)
    tensor<real32> output = tensor<real32>(input.shape(), gpu = nat(input.device()))
    int32 status = break_backward_native(&input, &output, kind)
    if status != int32(0)
        error("stream package break_backward failed with status {status}")
    return output
QUI
    cat > "$TMP/packages/stream_pkg/native/stream.mm" <<'MM'
#include <quidra/native_extension.h>
#import <Metal/Metal.h>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
std::atomic<int> completed{0};

void note_completion(void*) { completed.fetch_add(1); }

id<MTLComputePipelineState> pipeline(id<MTLDevice> device) {
    static id<MTLComputePipelineState> cached = nil;
    if (cached) return cached;
    NSString* source =
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void scale(device const float* in [[buffer(0)]],"
        " device float* out [[buffer(1)]], constant float& factor [[buffer(2)]],"
        " uint gid [[thread_position_in_grid]]) { out[gid] = in[gid] * factor; }\n";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return nil;
    id<MTLFunction> function = [library newFunctionWithName:@"scale"];
    cached = [device newComputePipelineStateWithFunction:function error:&error];
    [function release];
    [library release];
    return cached;
}

id<MTLBuffer> buffer_of(uint64_t handle) {
    return (__bridge id<MTLBuffer>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(handle));
}

void encode(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> state,
            const void* input, void* output, uint64_t in_handle,
            uint64_t out_handle, float factor) {
    [encoder setComputePipelineState:state];
    [encoder setBuffer:buffer_of(in_handle)
                offset:qcore_tensor_device_offset_bytes(input) atIndex:0];
    [encoder setBuffer:buffer_of(out_handle)
                offset:qcore_tensor_device_offset_bytes(output) atIndex:1];
    [encoder setBytes:&factor length:sizeof(factor) atIndex:2];
    const auto count = qcore_tensor_element_count(input);
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
}
} // namespace

int32_t legacy(const void* input, void* output, float factor, long long device) {
    // Unmigrated: own command buffer on Core's queue, committed, not waited.
    const auto in_handle = qcore_tensor_device_handle_const(input);
    const auto out_handle = qcore_tensor_output_handle(output);
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(qcore_device_queue_handle(device)));
    if (!in_handle || !out_handle || !queue) return 8;
    id<MTLComputePipelineState> state = pipeline([queue device]);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!state || !encoder) return 9;
    encode(encoder, state, input, output, in_handle, out_handle, factor);
    [encoder endEncoding];
    [command commit];
    return 0;
}

int32_t scale(const void* input, void* output, float factor, int32_t mode);

// Every element of the output is written: it is taken as a write-only output
// (qcore_tensor_output_handle) and marked written after the encode.
extern "C" int32_t qtest_stream_scale(const void* input, void* output,
                                      float factor, int32_t mode) {
    const int32_t status = scale(input, output, factor, mode);
    if (status != 0) return status;
    return qcore_tensor_mark_written(output) == 0 ? 0 : 10;
}

int32_t scale(const void* input, void* output, float factor, int32_t mode) {
    const auto device = qcore_tensor_device(input);
    if (qcore_tensor_backend(input) != QCORE_BACKEND_METAL) return 1;
    if (mode == 0 || mode == 1) {
        // The protocol: hold Core's stream, take every handle (Core may work
        // for them now), open the scope, set state, dispatch, close. Mode 0
        // dispatches on Core's encoder, mode 1 takes Core's command buffer
        // for an encoder of its own.
        if (!qcore_device_encode_begin(device)) {
            const int32_t status = legacy(input, output, factor, device);
            if (status != 0 || mode != 0) return status;
            return qcore_device_on_complete(device, note_completion, nullptr) == 0 ? 0 : 4;
        }
        const auto in_handle = qcore_tensor_device_handle_const(input);
        const auto out_handle = qcore_tensor_output_handle(output);
        id<MTLComputePipelineState> state = pipeline(
            (__bridge id<MTLDevice>)reinterpret_cast<void*>(
                static_cast<uintptr_t>(qcore_device_native_device(device))));
        if (!in_handle || !out_handle || !state) {
            qcore_metal_note_work(device, 0, 0);
            return 3;
        }
        if (mode == 0) {
            const auto encoder_handle = qcore_device_compute_encoder(device);
            if (encoder_handle == 0) {
                qcore_metal_note_work(device, 0, 0);
                return 3;
            }
            encode((__bridge id<MTLComputeCommandEncoder>)reinterpret_cast<void*>(
                       static_cast<uintptr_t>(encoder_handle)),
                   state, input, output, in_handle, out_handle, factor);
            qcore_metal_note_work(device, 1, 0);
            return qcore_device_on_complete(device, note_completion, nullptr) == 0 ? 0 : 4;
        }
        const auto command_handle = qcore_metal_command_buffer(device);
        id<MTLCommandBuffer> command =
            (__bridge id<MTLCommandBuffer>)reinterpret_cast<void*>(
                static_cast<uintptr_t>(command_handle));
        id<MTLComputeCommandEncoder> encoder =
            command_handle ? [command computeCommandEncoder] : nil;
        if (!encoder) {
            qcore_metal_note_work(device, 0, 0);
            return 7;
        }
        encode(encoder, state, input, output, in_handle, out_handle, factor);
        [encoder endEncoding];
        qcore_metal_note_work(device, 1, 0);
        return 0;
    }
    if (mode == 3 || mode == 4) {
        // Mode 3 takes the handles without a hold, before the scope: correct,
        // but each lend commits Core's open command buffer. Mode 4 opens the
        // scope first and takes the handles inside it, which only works for
        // lookups that need no Core work (a resident input, an output that
        // alone owns its storage).
        uint64_t in_handle = 0, out_handle = 0;
        if (mode == 3) {
            in_handle = qcore_tensor_device_handle_const(input);
            out_handle = qcore_tensor_output_handle(output);
            if (!in_handle || !out_handle) return 5;
        }
        const auto encoder_handle = qcore_device_compute_encoder(device);
        if (encoder_handle == 0) return legacy(input, output, factor, device);
        if (mode == 4) {
            in_handle = qcore_tensor_device_handle_const(input);
            out_handle = qcore_tensor_output_handle(output);
        }
        id<MTLComputeCommandEncoder> encoder =
            (__bridge id<MTLComputeCommandEncoder>)reinterpret_cast<void*>(
                static_cast<uintptr_t>(encoder_handle));
        id<MTLComputePipelineState> state = pipeline([encoder device]);
        if (!in_handle || !out_handle || !state) {
            qcore_metal_note_work(device, 0, 0);
            return 6;
        }
        encode(encoder, state, input, output, in_handle, out_handle, factor);
        qcore_metal_note_work(device, 1, 0);
        return 0;
    }
    return legacy(input, output, factor, device);
}

extern "C" int32_t qtest_stream_completions() { return completed.load(); }

// Flags a negative element through a Core status word, then defers the
// check to the next synchronization point or waits for it now.
extern "C" int32_t qtest_stream_check(const void* input, int32_t deferred) {
    const auto device = qcore_tensor_device(input);
    if (!qcore_device_encode_begin(device)) return 2;
    uint64_t status_buffer = 0, status_offset = 0, slot = 0;
    if (qcore_device_status_slot(device, &status_buffer, &status_offset, &slot) != 0) {
        qcore_metal_note_work(device, 0, 0);
        return 1;
    }
    const auto in_handle = qcore_tensor_device_handle_const(input);
    const auto encoder_handle = in_handle ? qcore_device_compute_encoder(device) : 0;
    if (!encoder_handle) {
        qcore_metal_note_work(device, 0, 0);
        qcore_device_status_release(device, slot);
        return 2;
    }
    id<MTLComputeCommandEncoder> encoder =
        (__bridge id<MTLComputeCommandEncoder>)reinterpret_cast<void*>(
            static_cast<uintptr_t>(encoder_handle));
    static id<MTLComputePipelineState> state = nil;
    if (!state) {
        NSString* source =
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void check(device const float* in [[buffer(0)]],"
            " device atomic_uint* status [[buffer(1)]],"
            " uint gid [[thread_position_in_grid]]) {"
            " if (in[gid] < 0.0f) atomic_store_explicit(status, 1u, memory_order_relaxed); }\n";
        NSError* error = nil;
        id<MTLLibrary> library = [[encoder device] newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"check"];
        state = [[encoder device] newComputePipelineStateWithFunction:function error:&error];
        [function release];
        [library release];
    }
    [encoder setComputePipelineState:state];
    [encoder setBuffer:buffer_of(in_handle)
                offset:qcore_tensor_device_offset_bytes(input) atIndex:0];
    [encoder setBuffer:buffer_of(status_buffer) offset:status_offset atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(qcore_tensor_element_count(input), 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    qcore_metal_note_work(device, 1, 0);
    if (deferred)
        return qcore_device_defer_status(device, slot, "stream_pkg negative input") == 0 ? 0 : 3;
    uint32_t value = 0;
    if (qcore_device_status_wait(device, slot, &value) != 0) return 5;
    return value == 0 ? 0 : 4;
}

namespace {
std::atomic<int> slow_status{-1};

id<MTLComputePipelineState> slow_pipeline(id<MTLDevice> device) {
    static id<MTLComputePipelineState> cached = nil;
    static std::atomic<bool> busy{false};
    while (busy.exchange(true)) {}
    if (!cached) {
        NSString* source =
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void slow(device const float* in [[buffer(0)]],"
            " device float* out [[buffer(1)]], constant int& iterations [[buffer(2)]],"
            " uint gid [[thread_position_in_grid]]) { float v = in[gid]; float a = 0.0f;"
            " for (int k = 0; k < iterations; ++k) a = a * 0.5f + v;"
            " out[gid] = (a > 1e30f) ? a : v; }\n";
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"slow"];
        cached = [device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        [library release];
    }
    busy.store(false);
    return cached;
}

void note_slow_done(void* context) {
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)context;
    slow_status.store(static_cast<int>([command status]));
    [command release];
}
} // namespace

// Unmigrated slow kernel (output = input): its own command buffer on Core's
// queue, committed without waiting. mode 1 then asks Core for a completion
// callback, which records the command buffer's status; mode 2 prepares on
// the CPU for 3 ms between borrowing the handles and committing.
extern "C" int32_t qtest_stream_slow(const void* input, void* output,
                                     int32_t iterations, int32_t mode) {
    const auto device = qcore_tensor_device(input);
    const auto in_handle = qcore_tensor_device_handle_const(input);
    const auto out_handle = qcore_tensor_output_handle(output);
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(qcore_device_queue_handle(device)));
    if (!in_handle || !out_handle || !queue) return 2;
    id<MTLComputePipelineState> state = slow_pipeline([queue device]);
    if (!state) return 3;
    if (mode == 2) usleep(3000);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:state];
    [encoder setBuffer:buffer_of(in_handle)
                offset:qcore_tensor_device_offset_bytes(input) atIndex:0];
    [encoder setBuffer:buffer_of(out_handle)
                offset:qcore_tensor_device_offset_bytes(output) atIndex:1];
    [encoder setBytes:&iterations length:sizeof(iterations) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(qcore_tensor_element_count(input), 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    [command commit];
    if (mode == 1) {
        slow_status.store(-1);
        if (qcore_device_on_complete(device, note_slow_done,
                                     (__bridge void*)[command retain]) != 0)
            return 4;
    }
    return qcore_tensor_mark_written(output) == 0 ? 0 : 5;
}

extern "C" int32_t qtest_stream_slow_status() { return slow_status.load(); }

// Takes Core's queue handle in a hold, ends the hold (which commits Core's
// open work, so the package's own command buffers on the queue run after
// it), and later commits a slow command buffer of its own on that queue that
// writes a package-private buffer; qcore_device_wait must cover it before
// the host reads the result. Returns 1 when the host saw the finished result.
extern "C" int32_t qtest_stream_scope_queue(const void* input, int32_t iterations) {
    const auto device = qcore_tensor_device(input);
    if (!qcore_device_encode_begin(device)) return -1;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(qcore_device_queue_handle(device)));
    qcore_metal_note_work(device, 0, 0);
    if (!queue) return -2;
    id<MTLComputePipelineState> state = slow_pipeline([queue device]);
    if (!state) return -4;
    constexpr NSUInteger count = 262144;
    id<MTLBuffer> in = [[queue device] newBufferWithLength:count * sizeof(float)
                                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [[queue device] newBufferWithLength:count * sizeof(float)
                                                    options:MTLResourceStorageModeShared];
    float* host_in = static_cast<float*>([in contents]);
    for (NSUInteger i = 0; i < count; ++i) host_in[i] = 1.0f;
    std::memset([out contents], 0, count * sizeof(float));
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:state];
    [encoder setBuffer:in offset:0 atIndex:0];
    [encoder setBuffer:out offset:0 atIndex:1];
    [encoder setBytes:&iterations length:sizeof(iterations) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    [command commit];
    const int waited = qcore_device_wait(device);
    const float last = static_cast<const float*>([out contents])[count - 1];
    [in release];
    [out release];
    if (waited != 0) return -5;
    return last == 1.0f ? 1 : 0;
}

namespace {
std::atomic<int> late_state{0};

// Runs on Core's completion queue while the main thread waits in a
// synchronization: queues a failing check on Core's stream and defers it.
void late_check(void* context) {
    const long long device = static_cast<long long>(reinterpret_cast<intptr_t>(context));
    if (!qcore_device_encode_begin(device)) { late_state.store(1); return; }
    uint64_t status_buffer = 0, status_offset = 0, slot = 0;
    if (qcore_device_status_slot(device, &status_buffer, &status_offset, &slot) != 0) {
        qcore_metal_note_work(device, 0, 0);
        late_state.store(2);
        return;
    }
    const auto encoder_handle = qcore_device_compute_encoder(device);
    if (encoder_handle == 0) {
        qcore_metal_note_work(device, 0, 0);
        qcore_device_status_release(device, slot);
        late_state.store(1);
        return;
    }
    id<MTLComputeCommandEncoder> encoder =
        (__bridge id<MTLComputeCommandEncoder>)reinterpret_cast<void*>(
            static_cast<uintptr_t>(encoder_handle));
    static id<MTLComputePipelineState> state = nil;
    if (!state) {
        NSString* source =
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void fail(device atomic_uint* status [[buffer(0)]],"
            " uint gid [[thread_position_in_grid]]) {"
            " if (gid == 0) atomic_store_explicit(status, 1u, memory_order_relaxed); }\n";
        NSError* error = nil;
        id<MTLLibrary> library = [[encoder device] newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"fail"];
        state = [[encoder device] newComputePipelineStateWithFunction:function error:&error];
        [function release];
        [library release];
    }
    [encoder setComputePipelineState:state];
    [encoder setBuffer:buffer_of(status_buffer) offset:status_offset atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    qcore_metal_note_work(device, 1, 0);
    late_state.store(qcore_device_defer_status(device, slot, "stream_pkg late check") == 0 ? 3 : 4);
}
} // namespace

// Arms late_check to run when all work queued so far has completed.
extern "C" int32_t qtest_stream_arm_late_check(const void* input) {
    const auto device = qcore_tensor_device(input);
    late_state.store(0);
    return qcore_device_on_complete(device, late_check,
                                    reinterpret_cast<void*>(static_cast<intptr_t>(device)));
}

extern "C" int32_t qtest_stream_late_check_state() { return late_state.load(); }

namespace {
std::atomic<int> callback_state{0};

// Completion callbacks that call back into Core's stream: a wait (mode 0)
// or a flush (mode 1).
void wait_in_callback(void* context) {
    const long long device = static_cast<long long>(reinterpret_cast<intptr_t>(context));
    callback_state.store(qcore_device_wait(device) == 0 ? 31 : 32);
}
void flush_in_callback(void* context) {
    const long long device = static_cast<long long>(reinterpret_cast<intptr_t>(context));
    callback_state.store(qcore_device_flush(device) == 0 ? 21 : 22);
}
// Takes a hold and returns without qcore_metal_note_work (mode 2).
void leave_hold_in_callback(void* context) {
    const long long device = static_cast<long long>(reinterpret_cast<intptr_t>(context));
    callback_state.store(qcore_device_encode_begin(device) ? 41 : 42);
}
// Takes a hold, asks for a flush in it (refused) and ends the hold (mode 3).
void flush_in_hold_in_callback(void* context) {
    const long long device = static_cast<long long>(reinterpret_cast<intptr_t>(context));
    if (!qcore_device_encode_begin(device)) {
        callback_state.store(52);
        return;
    }
    const int flushed = qcore_device_flush(device);
    std::fprintf(stderr, "callback flush %d\n", flushed);
    std::fflush(stderr);
    callback_state.store(flushed == 0 ? 51 : 53);
    qcore_metal_note_work(device, 0, 0);
}
} // namespace

extern "C" int32_t qtest_stream_arm_callback(const void* input, int32_t mode) {
    const auto device = qcore_tensor_device(input);
    void* context = reinterpret_cast<void*>(static_cast<intptr_t>(device));
    callback_state.store(0);
    if (mode == 2) {
        // Registered in a hold, the callback rides on the command buffer
        // Core opens for it, so it never runs at once on this thread.
        if (!qcore_device_encode_begin(device)) return 1;
        const int status = qcore_device_on_complete(device, leave_hold_in_callback, context);
        qcore_metal_note_work(device, 0, 0);
        return status;
    }
    return qcore_device_on_complete(device,
                                    mode == 1 ? flush_in_callback
                                    : mode == 3 ? flush_in_hold_in_callback
                                                : wait_in_callback,
                                    context);
}

extern "C" int32_t qtest_stream_callback_state() { return callback_state.load(); }

// Runtime counter hooks: step marks, a refusal report and counter
// reads by name (0 for an unknown name or when counting is off).
extern "C" int32_t qtest_stream_hooks(int32_t which) {
    if (which == 0) {
        qcore_counters_mark("stream_pkg setup");
        return 0;
    }
    if (which == 1) {
        qcore_counters_mark("stream_pkg loop");
        return static_cast<int32_t>(qcore_counter_value("package_encode_scopes"));
    }
    if (which == 2) {
        qcore_counter_note_refusal("stream_pkg", "scale", 7);
        return static_cast<int32_t>(qcore_counter_value("package_refusals"));
    }
    if (which == 3) {
        qcore_counters_mark("stream_pkg inplace start");
        return 0;
    }
    if (which == 4) {
        qcore_counters_mark("stream_pkg inplace done");
        return static_cast<int32_t>(qcore_counter_value("package_scope_violations"));
    }
    return static_cast<int32_t>(qcore_counter_value("no_such_counter"));
}

// Core scratch written on the host right after allocation, then copied into
// the output by the package's own command buffer (unmigrated, waited).
extern "C" int32_t qtest_stream_scratch_fill(void* output, float value) {
    const auto device = qcore_tensor_device(output);
    const auto count = qcore_tensor_element_count(output);
    const auto bytes = count * sizeof(float);
    void* token = qcore_device_buffer_allocate(device, bytes);
    if (!token) return 1;
    const auto scratch_handle = qcore_device_buffer_handle(token);
    const auto out_handle = qcore_tensor_device_handle(output);
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(qcore_device_queue_handle(device)));
    if (!scratch_handle || !out_handle || !queue) {
        qcore_device_buffer_release(token);
        return 2;
    }
    id<MTLBuffer> scratch = buffer_of(scratch_handle);
    float* host = static_cast<float*>([scratch contents]);
    for (uint64_t i = 0; i < count; ++i) host[i] = value;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromBuffer:scratch sourceOffset:0 toBuffer:buffer_of(out_handle)
       destinationOffset:qcore_tensor_device_offset_bytes(output) size:bytes];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    qcore_device_buffer_release(token);
    return 0;
}

// qcore_device_wait for the device of `input`: 0 on success.
extern "C" int32_t qtest_stream_device_wait(const void* input) {
    return qcore_device_wait(qcore_tensor_device(input)) == 0 ? 0 : 1;
}

// Takes a status slot while holding the stream (the migrated pattern takes
// it there, before the scope opens), encodes nothing with it and releases it.
extern "C" int32_t qtest_stream_scope_status(const void* input) {
    const auto device = qcore_tensor_device(input);
    if (!qcore_device_encode_begin(device)) return 1;
    uint64_t status_buffer = 0, status_offset = 0, slot = 0;
    const int status = qcore_device_status_slot(device, &status_buffer,
                                                &status_offset, &slot);
    qcore_metal_note_work(device, 0, 0);
    if (status != 0) return 2;
    qcore_device_status_release(device, slot);
    return 0;
}

namespace {
id<MTLComputePipelineState> increment_pipeline(id<MTLDevice> device) {
    static std::mutex mutex;
    static id<MTLComputePipelineState> cached = nil;
    std::lock_guard<std::mutex> lock(mutex);
    if (cached) return cached;
    NSString* source =
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void increment(device float* io [[buffer(0)]],"
        " constant float& delta [[buffer(1)]], constant uint& count [[buffer(2)]],"
        " uint gid [[thread_position_in_grid]]) {"
        " if (gid < count) io[gid] = io[gid] + delta; }\n";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return nil;
    id<MTLFunction> function = [library newFunctionWithName:@"increment"];
    cached = [device newComputePipelineStateWithFunction:function error:&error];
    [function release];
    [library release];
    return cached;
}

id<MTLDevice> metal_device(long long device) {
    return (__bridge id<MTLDevice>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(qcore_device_native_device(device)));
}

void encode_increment(id<MTLComputeCommandEncoder> encoder,
                      id<MTLComputePipelineState> state, uint64_t handle,
                      uint64_t offset, uint32_t count) {
    const float delta = 1.0f;
    [encoder setComputePipelineState:state];
    [encoder setBuffer:buffer_of(handle) offset:offset atIndex:0];
    [encoder setBytes:&delta length:sizeof(delta) atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
}

int32_t dummy_backward(const void* const*, uint64_t, const void*, void* const*,
                       uint64_t, const void*, uint64_t) {
    return 0;
}
} // namespace

// Adds 1 in place through the mutable handle of a tensor whose storage the
// caller still shares, so the handle needs a copy-on-write detach, which is
// Core work. Mode 0 takes it while holding the stream, before the encoder
// scope opens (Core detaches on encoders of its own, a blit one in the blit
// upload mode), then dispatches on Core's encoder; mode 1 does the same and
// then takes Core's command buffer for an encoder of its own; mode 2 takes
// it without a hold (the lend commits Core's batch) and then opens the
// scope.
extern "C" int32_t qtest_stream_inplace(void* tensor, int32_t mode) {
    const auto device = qcore_tensor_device(tensor);
    id<MTLComputePipelineState> state = increment_pipeline(metal_device(device));
    if (!state) return 1;
    const bool held = mode != 2;
    if (held && !qcore_device_encode_begin(device)) return 1;
    const auto handle = qcore_tensor_device_handle(tensor);
    const auto offset = qcore_tensor_device_offset_bytes(tensor);
    const auto count = static_cast<uint32_t>(qcore_tensor_element_count(tensor));
    const uint64_t scope = !handle ? 0
        : mode == 1 ? qcore_metal_command_buffer(device)
                    : qcore_device_compute_encoder(device);
    if (scope == 0) {
        if (held) qcore_metal_note_work(device, 0, 0);
        return 2;
    }
    if (mode == 1) {
        id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)
            reinterpret_cast<void*>(static_cast<uintptr_t>(scope));
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        encode_increment(encoder, state, handle, offset, count);
        [encoder endEncoding];
    } else {
        encode_increment((__bridge id<MTLComputeCommandEncoder>)
                             reinterpret_cast<void*>(static_cast<uintptr_t>(scope)),
                         state, handle, offset, count);
    }
    qcore_metal_note_work(device, 1, 0);
    return 0;
}

// Protocol violations. Each refused call is reported on stderr as
// "refused <call> <value>" (the failure value the package receives), and
// qcore_metal_note_work then stops the program (GPU_SCOPE) without running
// anything of the scope. `t` shares its storage with the caller.
//   0  qcore_tensor_device_handle of `t` inside the encoder scope
//   1  the same inside a command-buffer scope; the package ignores the
//      refusal, dispatches with a nil binding and leaves its encoder open
//   2  qcore_device_status_slot inside the encoder scope
//   3  qcore_tensor_output_handle of `t` inside the encoder scope
//   4  a second qcore_device_compute_encoder inside the encoder scope
//   5  qcore_device_encode_begin while holding the stream
//   6  qcore_device_flush while holding the stream (no scope yet)
//   7  qcore_device_wait inside the encoder scope
//   8  a custom autograd attach inside the encoder scope
//   9  the encoder scope is left open: the call returns without
//      qcore_metal_note_work, which the program's next statement reports
//  10  qcore_tensor_device_handle_const inside the encoder scope of a `t`
//      that is a unified-memory view its CPU source still shares
//  11  qcore_device_flush for another device while holding the stream
//  12  qcore_device_queue_handle for another device inside the scope
extern "C" int32_t qtest_stream_violate(void* t, int32_t kind) {
    const auto device = qcore_tensor_device(t);
    id<MTLComputePipelineState> state = increment_pipeline(metal_device(device));
    if (!state) return 1;
    auto report = [](const char* call, long long value) {
        std::fprintf(stderr, "refused %s %lld\n", call, value);
        std::fflush(stderr);
    };
    if (kind == 5 || kind == 6 || kind == 11) {
        if (!qcore_device_encode_begin(device)) return 2;
        if (kind == 5)
            report("qcore_device_encode_begin", qcore_device_encode_begin(device));
        else
            report("qcore_device_flush",
                   qcore_device_flush(kind == 11 ? device + 1 : device));
        qcore_metal_note_work(device, 0, 0);
        return 0;
    }
    if (kind == 1) {
        const auto command_handle = qcore_metal_command_buffer(device);
        if (command_handle == 0) return 2;
        const auto handle = qcore_tensor_device_handle(t);
        report("qcore_tensor_device_handle", static_cast<long long>(handle));
        id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)
            reinterpret_cast<void*>(static_cast<uintptr_t>(command_handle));
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        encode_increment(encoder, state, handle, 0,
                         static_cast<uint32_t>(qcore_tensor_element_count(t)));
        qcore_metal_note_work(device, 1, 0);
        return 0;
    }
    const auto encoder_handle = qcore_device_compute_encoder(device);
    if (encoder_handle == 0) return 2;
    switch (kind) {
        case 0:
            report("qcore_tensor_device_handle",
                   static_cast<long long>(qcore_tensor_device_handle(t)));
            break;
        case 2: {
            uint64_t buffer = 0, offset = 0, slot = 0;
            report("qcore_device_status_slot",
                   qcore_device_status_slot(device, &buffer, &offset, &slot));
            break;
        }
        case 3:
            report("qcore_tensor_output_handle",
                   static_cast<long long>(qcore_tensor_output_handle(t)));
            break;
        case 4:
            report("qcore_device_compute_encoder",
                   static_cast<long long>(qcore_device_compute_encoder(device)));
            break;
        case 7:
            report("qcore_device_wait", qcore_device_wait(device));
            break;
        case 8: {
            const void* inputs[] = {t};
            report("qcore_tensor_attach_custom_autograd",
                   qcore_tensor_attach_custom_autograd(t, inputs, 1,
                                                       dummy_backward, nullptr, 0));
            break;
        }
        case 9:
            return 0;
        case 10:
            report("qcore_tensor_device_handle_const",
                   static_cast<long long>(qcore_tensor_device_handle_const(t)));
            break;
        case 12:
            report("qcore_device_queue_handle",
                   static_cast<long long>(qcore_device_queue_handle(device + 1)));
            break;
        default:
            break;
    }
    qcore_metal_note_work(device, 0, 0);
    return 0;
}

// Mutable CPU pointer of `host`, a CPU tensor whose unified memory a device
// view (now gone) may still be read by queued work, next to a device handle
// of `input`. Mode 0 takes it while holding the stream, which is refused
// whether or not that work has finished (a hold neither commits nor waits);
// mode 1 takes a const pointer in the hold instead; mode 2 takes the mutable
// pointer before the hold. Prints "pointer <0|1>".
extern "C" int32_t qtest_stream_cpu_borrow(const void* input, void* host, int32_t mode) {
    const auto device = qcore_tensor_device(input);
    const void* early = mode == 2 ? qcore_tensor_cpu_data(host) : nullptr;
    if (!qcore_device_encode_begin(device)) return 2;
    const auto handle = qcore_tensor_device_handle_const(input);
    const void* pointer = mode == 0 ? qcore_tensor_cpu_data(host)
                        : mode == 1 ? qcore_tensor_cpu_data_const(host)
                                    : early;
    std::fprintf(stderr, "pointer %d\n", pointer != nullptr ? 1 : 0);
    std::fflush(stderr);
    qcore_metal_note_work(device, 0, 0);
    return handle && pointer ? 0 : 3;
}

// Leaves a package encode open and returns: kind 0 the encoder scope, 1 the
// command-buffer scope, 2 the hold only.
extern "C" int32_t qtest_stream_leak(const void* t, int32_t kind) {
    const auto device = qcore_tensor_device(t);
    if (kind == 2) return qcore_device_encode_begin(device) ? 0 : 2;
    if (kind == 1) return qcore_metal_command_buffer(device) ? 0 : 2;
    return qcore_device_compute_encoder(device) ? 0 : 2;
}

namespace {
std::atomic<int> task_hold_taken{0};
} // namespace

extern "C" int32_t qtest_stream_task_hold(const void* t, int32_t mode) {
    const auto device = qcore_tensor_device(t);
    if (!qcore_device_encode_begin(device)) return 2;
    task_hold_taken.store(1);
    usleep(200000); // the other task now waits for the held stream
    if (mode == 1) return 0;
    std::fprintf(stderr, "task flush %d\n", qcore_device_flush(device));
    std::fflush(stderr);
    qcore_metal_note_work(device, 0, 0);
    return 0;
}

extern "C" int32_t qtest_stream_await_hold() {
    for (int i = 0; i < 1000 && task_hold_taken.load() == 0; ++i) usleep(10000);
    return task_hold_taken.load() == 1 ? 0 : 1;
}

extern "C" int32_t qtest_stream_wait_report(const void* t) {
    const int waited = qcore_device_wait(qcore_tensor_device(t));
    std::fprintf(stderr, "reader waited %d\n", waited);
    std::fflush(stderr);
    return waited;
}

namespace {
std::atomic<int> backward_kind{0};

// Custom autograd backward that breaks the encode protocol (backward_kind):
// 0 leaves its hold open, 1 its encoder scope, 2 asks for a flush in the
// hold (refused) and ends the hold.
int32_t breaking_backward(const void* const*, uint64_t, const void* gradient_output,
                          void* const* gradient_inputs, uint64_t gradient_input_count,
                          const void*, uint64_t) {
    if (!gradient_output || !gradient_inputs || gradient_input_count != 1) return 70;
    const auto device = qcore_tensor_device(gradient_output);
    if (!qcore_device_encode_begin(device)) return 71;
    const int kind = backward_kind.load();
    if (kind == 1 && !qcore_device_compute_encoder(device)) return 72;
    if (kind == 2) {
        std::fprintf(stderr, "backward flush %d\n", qcore_device_flush(device));
        std::fflush(stderr);
        qcore_metal_note_work(device, 0, 0);
    }
    return 0;
}
} // namespace

extern "C" int32_t qtest_stream_break_backward(const void* input, void* output,
                                               int32_t kind) {
    backward_kind.store(kind);
    const int32_t status = qtest_stream_scale(input, output, 2.0f, 0);
    if (status != 0) return status;
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd(output, inputs, 1, breaking_backward,
                                               nullptr, 0);
}

namespace {
std::atomic<int> warmups{0};
std::atomic<int> late_warmups{0};
std::atomic<long long> warmed_device{-2};

// Package warm-up: compiles the increment pipeline for a device before its
// first use. Registered twice from a static initializer; the second
// registration is ignored.
void warm_up(long long device) {
    id<MTLDevice> metal = (__bridge id<MTLDevice>)reinterpret_cast<void*>(
        static_cast<uintptr_t>(qcore_device_native_device(device)));
    if (metal && increment_pipeline(metal)) warmed_device.store(device);
    warmups.fetch_add(1);
}

void warm_up_late(long long) { late_warmups.fetch_add(1); }

// A warm-up that breaks the encode protocol: it holds the stream, asks for
// a flush in the hold (refused) and ends the hold.
void warm_up_violate(long long device) {
    if (!qcore_device_encode_begin(device)) return;
    std::fprintf(stderr, "warm-up flush %d\n", qcore_device_flush(device));
    std::fflush(stderr);
    qcore_metal_note_work(device, 0, 0);
}

// A warm-up that asks for Core's queue (which takes the device stream)
// after the program has gone on: it waits while another thread holds the
// stream.
std::atomic<int> queue_warmup_started{0};
void warm_up_queue(long long device) {
    queue_warmup_started.store(1);
    usleep(300000);
    std::fprintf(stderr, "warm-up queue %d\n", qcore_device_queue_handle(device) != 0);
    std::fflush(stderr);
}

// A warm-up that is still running 2 s later: exit handlers that join the
// warm-up thread take that long.
std::atomic<int> sleep_warmup_started{0};
void warm_up_sleep(long long) {
    sleep_warmup_started.store(1);
    usleep(2000000);
}

struct RegisterWarmUp {
    RegisterWarmUp() {
        qcore_register_warmup(warm_up);
        qcore_register_warmup(warm_up);
    }
} register_warm_up;

bool wait_for(const std::atomic<int>& value, int target) {
    for (int i = 0; i < 1000 && value.load() < target; ++i) usleep(10000);
    return value.load() >= target;
}
} // namespace

// which 0: warm-up calls so far (0 before the program used a GPU).
// which 1: waits for the static warm-up of the input's device; 1 when it ran
// exactly once (still once 100 ms later) and compiled for that device.
// which 2: registers a new function, which runs for the device in use, and
// registers the static one again, which does not run again; 1 when so.
// which 3: registers warm_up_violate, which runs for the device in use.
// which 4: registers warm_up_queue and returns once it has started.
// which 5: registers warm_up_sleep and returns once it has started.
extern "C" int32_t qtest_stream_warmup_state(const void* input, int32_t which) {
    if (which == 0) return warmups.load() + late_warmups.load();
    if (which == 3) {
        qcore_register_warmup(warm_up_violate);
        return 0;
    }
    if (which == 4) {
        qcore_register_warmup(warm_up_queue);
        return wait_for(queue_warmup_started, 1) ? 0 : 7;
    }
    if (which == 5) {
        qcore_register_warmup(warm_up_sleep);
        return wait_for(sleep_warmup_started, 1) ? 0 : 8;
    }
    const auto device = qcore_tensor_device(input);
    if (which == 1) {
        if (!wait_for(warmups, 1)) return 2;
        usleep(100000);
        if (warmups.load() != 1) return 3;
        return warmed_device.load() == device ? 1 : 4;
    }
    qcore_register_warmup(warm_up_late);
    qcore_register_warmup(warm_up);
    if (!wait_for(late_warmups, 1)) return 5;
    usleep(100000);
    return late_warmups.load() == 1 && warmups.load() == 1 ? 1 : 6;
}
MM
    cat > "$TMP/stream-use.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * 2.0
tensor<real32> b = package.scale(a, real32(3), int32(0))
tensor<real32> c = b + 1.0
tensor<real32> d = package.scale(c, real32(2), int32(1))
tensor<real32> e = d - 1.0
tensor<real32> f = package.scale(e, real32(0.5), int32(2))
print(f.cpu()[4095].item())
print(NL)
tensor<real32> g = f * 4.0
print(g[5].item())
print(NL)
gpu.sync($GPU_INDEX)
print(package.completions())
print(NL)
QUI
    cat > "$TMP/stream-check.qui" <<QUI
import package = stream_pkg
tensor<real32> x = tensor.ones<real32>([16], gpu = $GPU_INDEX) - 2.0
print(package.check(x, int32(0)))
print(NL)
print(package.check(tensor.ones<real32>([16], gpu = $GPU_INDEX), int32(1)))
print(NL)
print(package.check(x, int32(1)))
print(NL)
tensor<real32> y = x * 2.0
print(y[0].item())
print(NL)
QUI
    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-check.qui" >"$TMP/stream-check.out" 2>"$TMP/stream-check.err"
    check_status=$?
    set -e
    # Deferred validation reports at the next host read (line 10), while
    # preserving the origin of the package check (line 7). It no longer
    # claims that the failure occurred in the package's source file.
    if [[ $check_status -ne 101 || "$(cat "$TMP/stream-check.out")" != "$(printf '4\n0\n0')" ]] ||
       ! grep -Fq "Quidra runtime error[TENSOR]" "$TMP/stream-check.err" ||
       ! grep -Fq "stream_pkg negative input (deferred GPU check from" "$TMP/stream-check.err" ||
       ! grep -Fq "stream-check.qui:7:1)" "$TMP/stream-check.err" ||
       ! grep -Fq "stream-check.qui:10:7" "$TMP/stream-check.err"; then
        echo "deferred package status check was not reported at the next host read (status $check_status)" >&2
        cat "$TMP/stream-check.out" "$TMP/stream-check.err" >&2
        exit 1
    fi

    stream_expected="$(printf '6.5\n26.0\n1')"
    for setting in 1 0; do
        stream_output="$(QUIDRA_TEST_POOL_POISON=0x7f QUIDRA_METAL_STREAM=$setting QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-use.qui" 2>&1 || true)"
        if [[ "$stream_output" != "$stream_expected" ]]; then
            echo "package encode on Core's Metal stream mismatch (QUIDRA_METAL_STREAM=$setting):" >&2
            printf '%s\n' "$stream_output" >&2
            exit 1
        fi
    done

    # Batching gate for migrated packages: a loop of Core multiply, package
    # scale and Core add. The protocol (hold, handles, scope: mode 0 with
    # Core's encoder, mode 1 with Core's command buffer) and handles taken
    # inside the scope as pure lookups (mode 4) append to Core's command
    # buffer without a borrow flush; handles taken without a hold before the
    # scope (mode 3) and the unmigrated path (mode 2) commit Core's batch at
    # every call. Idle commits are disabled so the command buffer count
    # depends only on batching. The run also exercises the counter hooks
    # (marks, a refusal report, reads by name).
    cat > "$TMP/stream-batch.qui" <<QUI
import package = stream_pkg
cli args
    int mode = option(default = 0)
tensor<real32> x = tensor.ones<real32>([4096], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
int32 setup = package.hooks(int32(0))
for i in range(200)
    tensor<real32> y = x * 1.0001
    tensor<real32> z = package.scale(y, real32(1), int32(args.mode))
    x = z + 0.0
int32 scopes = package.hooks(int32(1))
int32 refusals = package.hooks(int32(2))
int32 unknown = package.hooks(int32(3))
print("{scopes} {refusals} {unknown} {x[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-batch.qui" -o "$TMP/stream-batch" >/dev/null
    batch_value=""
    for mode in 0 1 4 3 2; do
        batch_output="$(QUIDRA_COUNTERS="$TMP/batch-$mode.jsonl" QUIDRA_METAL_IDLE_OPS=100000 QUIDRA_METAL_IDLE_NS=10000000000 "$TMP/stream-batch" --mode $mode)"
        expected_scopes=200
        [[ "$mode" == "2" ]] && expected_scopes=0
        value="${batch_output##* }"
        if [[ "${batch_output% *}" != "$expected_scopes 1 0" ]] ||
           [[ -n "$batch_value" && "$value" != "$batch_value" ]]; then
            echo "package batching loop output mismatch (mode $mode): '$batch_output'" >&2
            exit 1
        fi
        batch_value="$value"
        python3 - "$TMP/batch-$mode.jsonl" "$mode" <<'PY'
import json, sys
records = [json.loads(line) for line in open(sys.argv[1])]
mode = int(sys.argv[2])
loop = [r for r in records if r.get("label") == "stream_pkg loop"]
assert len(loop) == 1, records
loop = loop[0]
total = [r for r in records if r["record"] == "total"][0]
assert loop["package_scope_violations"] == 0, loop
assert loop["package_encode_holds"] == (200 if mode in (0, 1) else 0), loop
if mode in (0, 1, 4):
    assert loop["package_borrow_flushes"] == 0, loop
    assert loop["package_queue_borrows"] == 0, loop
    assert loop["package_encode_scopes"] == 200, loop
    # 600 operations, committed every 100.
    assert loop["metal_command_buffers"] <= 8, loop
    assert loop["metal_marker_buffers"] == 0, loop
else:
    assert loop["package_borrow_flushes"] >= 200, loop
    assert loop["metal_command_buffers"] >= 200, loop
assert total["package_refusals"] == 1, total
assert total["refusals"] == {"stream_pkg/scale:7": 1}, total
assert any(r.get("label") == "stream_pkg setup" for r in records), records
PY
    done

    # Runs a program with a deadline; a hang fails the suite instead of
    # blocking it. Prints the exit status (124 on timeout).
    run_with_deadline() {
        local seconds="$1" output="$2"; shift 2
        "$@" >"$output" 2>&1 &
        local pid=$!
        local waited=0
        while kill -0 "$pid" 2>/dev/null; do
            if (( waited >= seconds * 10 )); then
                kill -9 "$pid" 2>/dev/null
                wait "$pid" 2>/dev/null
                echo 124
                return
            fi
            sleep 0.1
            waited=$((waited + 1))
        done
        set +e
        wait "$pid"
        local status=$?
        set -e
        echo "$status"
    }

    # A deferred check queued by one task.all thread is reported even while
    # another thread synchronizes in a loop: a synchronization consumes only
    # the checks whose work it waited for, so the check is not lost.
    cat > "$TMP/stream-lost-int.qui" <<QUI
real64 reader()
    tensor<real32> big = tensor.ones<real32>([1048576], gpu = $GPU_INDEX)
    real64 total = 0.0
    for i in range(300)
        tensor<real32> t = big * 1.0001
        total = total + real64(t[0].item())
    return total

real64 overflower()
    real64 spin = 0.0
    for k in range(200000)
        spin = spin + 1.0
    tensor<int8> values = tensor.ones<int8>([16], gpu = $GPU_INDEX) * int8(100)
    tensor<int8> sum = values + values
    print("overflow read {sum[0].item()}")
    print(NL)
    return spin

fn<real64>()[] operations = [reader, overflower]
real64[] results = task.all(operations)
print("finished without reporting the overflow")
print(NL)
QUI
    cat > "$TMP/stream-lost-check.qui" <<QUI
import package = stream_pkg

real64 reader()
    tensor<real32> big = tensor.ones<real32>([1048576], gpu = $GPU_INDEX)
    real64 total = 0.0
    for i in range(300)
        tensor<real32> t = big * 1.0001
        total = total + real64(t[0].item())
    return total

real64 checker()
    real64 spin = 0.0
    for k in range(200000)
        spin = spin + 1.0
    tensor<real32> negative = tensor.ones<real32>([16], gpu = $GPU_INDEX) - 2.0
    int32 status = package.check(negative, int32(1))
    tensor<real32> y = negative * 2.0
    print("checker read {y[0].item()}")
    print(NL)
    return spin

fn<real64>()[] operations = [reader, checker]
real64[] results = task.all(operations)
print("finished without reporting the deferred check")
print(NL)
QUI
    for run in 1 2; do
        status="$(run_with_deadline 120 "$TMP/lost-int.out" "$QUIDRA" run "$TMP/stream-lost-int.qui")"
        if [[ "$status" != "101" ]] || ! grep -Fq "tensor integer arithmetic overflow (deferred GPU check from" "$TMP/lost-int.out"; then
            echo "a concurrent synchronization lost a deferred integer check (run $run, status $status)" >&2
            cat "$TMP/lost-int.out" >&2
            exit 1
        fi
        status="$(run_with_deadline 120 "$TMP/lost-check.out" env QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-lost-check.qui")"
        if [[ "$status" != "101" ]] || ! grep -Fq "stream_pkg negative input (deferred GPU check from" "$TMP/lost-check.out"; then
            echo "a concurrent synchronization lost a deferred package check (run $run, status $status)" >&2
            cat "$TMP/lost-check.out" >&2
            exit 1
        fi
    done

    # Deterministic form of the same race: a check deferred from a completion
    # callback while the main thread waits in a read. The callback rides on
    # a marker behind a slow package kernel (tens of milliseconds), so it
    # runs after the read, a few microseconds later, committed what its wait
    # covers. The callback's check is queued after that, so the read must
    # not consume it (the read also returns only after the callback ran);
    # the next gpu.sync reports it.
    cat > "$TMP/stream-late-check.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([262144], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
tensor<real32> second = package.slow(a, int32(100000), int32(0))
int32 armed = package.arm_late_check(a)
print("{armed} {second.cpu()[0].item()} {package.late_check_state()}")
print(NL)
gpu.sync($GPU_INDEX)
print("late check lost")
print(NL)
QUI
    status="$(run_with_deadline 120 "$TMP/late-check.out" env QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-late-check.qui")"
    if [[ "$status" != "101" ]] || ! grep -Fxq "0 1.0 3" "$TMP/late-check.out" ||
       ! grep -Fq "stream_pkg late check" "$TMP/late-check.out"; then
        echo "a read consumed a deferred check queued after its wait started (status $status)" >&2
        cat "$TMP/late-check.out" >&2
        exit 1
    fi

    # The same callback armed at the end of the program: the exit guard waits
    # for the callback, whose check it must still report (language.md:
    # teardown reports any deferred checked-GPU failure) instead of exiting
    # with status 0.
    cat > "$TMP/stream-exit-check.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([262144], gpu = $GPU_INDEX)
tensor<real32> c = tensor.ones<real32>([8388608], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
for i in range(40)
    c = c * 1.0001
int32 armed = package.arm_late_check(a)
print("armed {armed}")
print(NL)
QUI
    for run in 1 2; do
        status="$(run_with_deadline 120 "$TMP/exit-check.out" env QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-exit-check.qui")"
        if [[ "$status" != "101" ]] || ! grep -Fq "GPU_ASYNC" "$TMP/exit-check.out" ||
           ! grep -Fq "stream_pkg late check" "$TMP/exit-check.out"; then
            echo "a check deferred by a completion callback during exit was lost (run $run, status $status)" >&2
            cat "$TMP/exit-check.out" >&2
            exit 1
        fi
    done

    # A package that asks for a status slot while it holds the stream and a
    # Core checked operation on another thread needs a new status page. Both
    # take the device stream before the validation state, so they cannot
    # deadlock.
    cat > "$TMP/stream-scope-status.qui" <<QUI
import package = stream_pkg

real64 package_loop()
    tensor<real32> x = tensor.ones<real32>([64], gpu = $GPU_INDEX)
    int32 total = int32(0)
    for i in range(30000)
        total = total + package.scope_status(x)
    return real64(total)

real64 checked_loop()
    tensor<int32> a = tensor.ones<int32>([64], gpu = $GPU_INDEX)
    for i in range(30000)
        a = a + int32(1)
    return real64(a[0].item())

fn<real64>()[] operations = [package_loop, checked_loop]
real64[] results = task.all(operations)
print("{results[0]} {results[1]}")
print(NL)
QUI
    for run in 1 2; do
        status="$(run_with_deadline 120 "$TMP/scope-status.out" env QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-scope-status.qui")"
        if [[ "$status" != "0" || "$(cat "$TMP/scope-status.out")" != "0.0 30001.0" ]]; then
            echo "status slots in a package hold deadlocked or failed (run $run, status $status)" >&2
            cat "$TMP/scope-status.out" >&2
            exit 1
        fi
    done

    # A package that commits its own command buffer after another thread's
    # synchronization is still covered by its own thread's next read: every
    # synchronization after a lend commits a fresh command buffer.
    cat > "$TMP/stream-lend-race.qui" <<QUI
import package = stream_pkg

real64 reader()
    tensor<real32> base = tensor.zeros<real32>([1048576], gpu = $GPU_INDEX)
    int stale = 0
    for i in range(60)
        real32 step = real32(i)
        tensor<real32> x = base + step
        tensor<real32> y = package.slow(x, int32(200), int32(2))
        tensor<real32> host = y.cpu()
        if host[0].item() != step
            stale += 1
    return real64(stale)

real64 syncer()
    real64 total = 0.0
    for i in range(400000)
        gpu.sync($GPU_INDEX)
        total = total + 1.0
    return total

fn<real64>()[] operations = [reader, syncer]
real64[] results = task.all(operations)
print(results[0])
print(NL)
QUI
    status="$(run_with_deadline 300 "$TMP/lend-race.out" env QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-lend-race.qui")"
    if [[ "$status" != "0" || "$(cat "$TMP/lend-race.out")" != "0.0" ]]; then
        echo "a read missed package work committed after another thread's synchronization (status $status)" >&2
        cat "$TMP/lend-race.out" >&2
        exit 1
    fi

    # Completion callbacks run on Core's completion queue, not inside Metal's
    # completion handler, so a callback may wait for the device (mode 0), and
    # may flush while the host is more than 64 command buffers ahead of a slow
    # package kernel (mode 1, one command buffer per operation): a command
    # buffer slot is freed only after its Metal completion handler returns.
    # Neither mode may hang.
    cat > "$TMP/stream-callback.qui" <<QUI
import package = stream_pkg
cli args
    int mode = option(default = 0)
    int ops = option(default = 10)
tensor<real32> a = tensor.ones<real32>([262144], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
tensor<real32> s = package.slow(a, int32(100000), int32(0))
int32 armed = package.arm_callback(a, int32(args.mode))
tensor<real32> v = tensor.ones<real32>([64], gpu = $GPU_INDEX)
for i in range(args.ops)
    v = v + 1.0
gpu.sync($GPU_INDEX)
print("{armed} {v[0].item()} {s[0].item()} {package.callback_state()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-callback.qui" -o "$TMP/stream-callback" >/dev/null
    status="$(run_with_deadline 120 "$TMP/callback-wait.out" "$TMP/stream-callback" --mode 0 --ops 10)"
    if [[ "$status" != "0" ]] || ! grep -Fxq "0 11.0 1.0 31" "$TMP/callback-wait.out"; then
        echo "a completion callback that waits for the device hung or failed (status $status)" >&2
        cat "$TMP/callback-wait.out" >&2
        exit 1
    fi
    status="$(run_with_deadline 120 "$TMP/callback-flush.out" env QUIDRA_METAL_STREAM=0 "$TMP/stream-callback" --mode 1 --ops 400)"
    if [[ "$status" != "0" ]] || ! grep -Fxq "0 401.0 1.0 21" "$TMP/callback-flush.out"; then
        echo "a completion callback that flushes deadlocked against a full command queue (status $status)" >&2
        cat "$TMP/callback-flush.out" >&2
        exit 1
    fi

    # A buffer handle taken in a package hold is recorded as used by Core's
    # command buffer, not as exposed to untracked package command buffers:
    # once that command buffer completed, a host write goes straight to
    # shared storage and a read needs no synchronization, instead of every
    # later host access taking the staged or synchronizing path. A queue
    # handle taken in a hold still makes later synchronizations cover a
    # command buffer the package commits after the hold, so the package's
    # wait does not return before its own slow command buffer finishes.
    cat > "$TMP/stream-scope-lend.qui" <<QUI
import package = stream_pkg
tensor<real32> x = tensor.ones<real32>([4096], gpu = $GPU_INDEX)
tensor<real32> z = package.scale(x, real32(3), int32(0))
gpu.sync($GPU_INDEX)
int32 mark = package.hooks(int32(0))
z[0] = real32(5)
print("{z[0].item()} {z[1].item()}")
print(NL)
int32 mark2 = package.hooks(int32(1))
QUI
    lend_output="$(QUIDRA_COUNTERS="$TMP/scope-lend.jsonl" QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-scope-lend.qui")"
    if [[ "$lend_output" != "5.0 3.0" ]]; then
        echo "host access to a package output written in a scope mismatch: '$lend_output'" >&2
        exit 1
    fi
    python3 - "$TMP/scope-lend.jsonl" <<'PY'
import json, sys
records = [json.loads(line) for line in open(sys.argv[1])]
step = [r for r in records if r.get("label") == "stream_pkg loop"][0]
assert step["metal_direct_uploads"] == 1 and step["metal_staged_uploads"] == 0, step
assert step["metal_command_buffers"] == 0 and step["host_waits"] == 0, step
PY
    cat > "$TMP/stream-scope-queue.qui" <<QUI
import package = stream_pkg
tensor<real32> x = tensor.ones<real32>([16], gpu = $GPU_INDEX)
print(package.scope_queue(x, int32(100000)))
print(NL)
QUI
    for run in 1 2; do
        queue_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-scope-queue.qui")"
        if [[ "$queue_output" != "1" ]]; then
            echo "a wait missed a package command buffer on a queue taken in a scope (run $run): '$queue_output'" >&2
            exit 1
        fi
    done

    # Package scratch (qcore_device_buffer_allocate) is idle storage: the
    # package writes it on the host at once, while a temporary of the same
    # size, freed just before, still waits for its kernel in Core's queue.
    # The queued kernel must not overwrite the scratch ('bad 2' or 'bad 3').
    cat > "$TMP/stream-scratch.qui" <<QUI
import package = stream_pkg
tensor<real32> c = tensor.ones<real32>([8388608], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
int bad = 0
int[] sizes = [3000, 12000, 48000, 196608, 786432, 3000000]
for n in sizes
    tensor<real32> a = tensor.ones<real32>([nat(n)], gpu = $GPU_INDEX)
    tensor<real32> out = tensor.zeros<real32>([nat(n)], gpu = $GPU_INDEX)
    gpu.sync($GPU_INDEX)
    for i in range(30)
        c = c * 1.0001
    tensor<real32> y = (a * 2.0) + 1.0
    out = package.scratch_fill(out, real32(7))
    tensor<real32> host = out.cpu()
    if host[0].item() != real32(7) or host[n - 1].item() != real32(7)
        bad += 1
print("bad {bad}")
print(NL)
QUI
    for setting in "" 0x5a; do
        scratch_output="$(QUIDRA_TEST_POOL_POISON=$setting QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-scratch.qui" 2>&1 || true)"
        if [[ "$scratch_output" != "bad 0" ]]; then
            echo "queued GPU work overwrote host-written package scratch (poison '$setting'):" >&2
            printf '%s\n' "$scratch_output" >&2
            exit 1
        fi
    done

    # The completion callback runs only after the package's own command
    # buffer completed (MTLCommandBufferStatusCompleted = 4).
    cat > "$TMP/stream-on-complete.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([262144], gpu = $GPU_INDEX)
gpu.sync($GPU_INDEX)
tensor<real32> b = package.slow(a, int32(20000), int32(1))
int32 at_call = package.slow_status()
gpu.sync($GPU_INDEX)
print(at_call == int32(-1) or at_call == int32(4))
print(NL)
print(package.slow_status())
print(NL)
QUI
    for setting in 1 0; do
        on_complete_output="$(QUIDRA_METAL_STREAM=$setting QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-on-complete.qui" 2>&1 || true)"
        if [[ "$on_complete_output" != "$(printf 'true\n4')" ]]; then
            echo "package completion callback ran before the package command buffer completed (QUIDRA_METAL_STREAM=$setting):" >&2
            printf '%s\n' "$on_complete_output" >&2
            exit 1
        fi
    done

    # Encoder ownership (native_extension.h): while a package holds Core's
    # encoder or command buffer, Core does no work on the stream, so the
    # mutable handle of a tensor the caller still shares is taken before the
    # scope opens. Its copy-on-write detach then runs on Core's own encoders:
    # in a hold (modes 0 and 1, no borrow flush) or without one (mode 2, the
    # lend commits Core's batch). Core work encoded on the package's encoder
    # would replace its pipeline state and bindings ('w 4.0 4.0' instead of
    # 3.0), and a blit encoder for the detach's index upload would end the
    # encoder the package holds (use-after-free, rc=139). With
    # QUIDRA_BROADCAST=gather the detach uploads gather indices, which the
    # blit upload mode encodes on a blit encoder before the scope.
    cat > "$TMP/stream-inplace.qui" <<QUI
import package = stream_pkg
cli args
    int mode = option(default = 0)
    int n = option(default = 4096)
int bad = 0
int32 violations = int32(0)
for i in range(20)
    real32 step = real32(i)
    real32 next_step = real32(i + 1)
    real32 doubled_step = real32(2 * (i + 1))
    tensor<real32> a = tensor.ones<real32>([nat(args.n)], gpu = $GPU_INDEX) * step
    int32 start = package.hooks(int32(3))
    tensor<real32> b = package.inplace(a, int32(args.mode))
    violations = violations + package.hooks(int32(4))
    tensor<real32> c = b * 2.0
    if a[0].item() != step or a[args.n - 1].item() != step
        bad += 1
    if b[0].item() != next_step or b[args.n - 1].item() != next_step
        bad += 100
    if c[args.n - 1].item() != doubled_step
        bad += 10000
print("bad {bad} {violations}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-inplace.qui" -o "$TMP/stream-inplace" >/dev/null
    for upload in default blit; do
        for broadcast in strided gather; do
            for mode in 0 1 2; do
                for size in 4096 1048576; do
                    tag="$upload-$broadcast-$mode-$size"
                    inplace_output="$(QUIDRA_METAL_UPLOAD=$upload QUIDRA_BROADCAST=$broadcast QUIDRA_COUNTERS="$TMP/inplace-$tag.jsonl" "$TMP/stream-inplace" --mode $mode --n $size 2>&1 || true)"
                    if [[ "$inplace_output" != "bad 0 0" ]]; then
                        echo "copy-on-write before a package encoder scope failed ($tag): '$inplace_output'" >&2
                        exit 1
                    fi
                done
            done
        done
    done
    for mode in 0 1 2; do
        inplace_output="$(QUIDRA_TEST_POOL_POISON=0x5a QUIDRA_COUNTERS="$TMP/inplace-poison-$mode.jsonl" "$TMP/stream-inplace" --mode $mode 2>&1 || true)"
        if [[ "$inplace_output" != "bad 0 0" ]]; then
            echo "copy-on-write before a package encoder scope failed under pool poisoning (mode $mode): '$inplace_output'" >&2
            exit 1
        fi
    done
    python3 - "$TMP"/inplace-*.jsonl <<'PY'
import json, os, sys
for path in sys.argv[1:]:
    tag = os.path.basename(path)[len("inplace-"):-len(".jsonl")]
    mode = int(tag.split("-")[-2] if not tag.startswith("poison") else tag.split("-")[-1])
    records = [json.loads(line) for line in open(path)]
    steps = [r for r in records if r.get("label") == "stream_pkg inplace done"]
    assert len(steps) == 20, (path, len(steps))
    for step in steps:
        # The detach is Core work: it ran before the scope, never on the
        # package's encoder, and nothing was refused.
        assert step["package_scope_violations"] == 0, (path, step)
        assert step["package_encode_scopes"] == 1, (path, step)
        assert step["core_dispatches"] + step["metal_blit_encoders"] >= 1, (path, step)
        if mode in (0, 1):
            # Lends in a hold never commit Core's batch.
            assert step["package_encode_holds"] == 1, (path, step)
            assert step["package_borrow_flushes"] == 0, (path, step)
        else:
            # Without a hold the lend commits Core's open work, unless an
            # idle commit already took it.
            assert step["package_encode_holds"] == 0, (path, step)
            assert step["package_borrow_flushes"] <= 1, (path, step)
        if tag.startswith("blit-gather"):
            # The gather-index upload of the detach is a blit, encoded
            # while the package holds the stream but before its scope.
            assert step["metal_blit_encoders"] >= 1, (path, step)
PY

    # Protocol violations (qtest_stream_violate): each refused call returns
    # its failure value to the package and does no Core work, and
    # qcore_metal_note_work discards the scope's command buffer (nothing of
    # it runs, also with a nil binding and an encoder the package left open)
    # and stops the program with GPU_SCOPE at the extern call. A scope left
    # open is reported the same way at the next statement. Kind 13 is kind 0
    # for a view of part of the storage, whose refusal names that reason,
    # not shared storage. Default and blit uploads, and pool poisoning, give
    # the same result.
    cat > "$TMP/stream-violate.qui" <<QUI
import package = stream_pkg
cli args
    int kind = option(default = 0)
tensor<real32> host = tensor.ones<real32>([65536]) * 2.0
tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * 2.0
if args.kind == 10
    a = host.gpu($GPU_INDEX)
tensor<real32> keep = a
print("before {a[0].item()}")
print(NL)
tensor<real32> b = a
if args.kind == 13
    b = package.violate(a[1:], int32(0))
else
    b = package.violate(a, int32(args.kind))
tensor<real32> c = b * 2.0
print("after {c[0].item()} {keep[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-violate.qui" -o "$TMP/stream-violate" >/dev/null
    other_gpu=$((GPU_INDEX + 1))
    violation_expect=(
        "refused qcore_tensor_device_handle 0|qcore_tensor_device_handle of a tensor that does not own its storage alone|holds Core's compute encoder"
        "refused qcore_tensor_device_handle 0|qcore_tensor_device_handle of a tensor that does not own its storage alone|holds Core's command buffer"
        "refused qcore_device_status_slot 1|qcore_device_status_slot (a slot may need Core to clear a status page)|holds Core's compute encoder"
        "refused qcore_tensor_output_handle 0|qcore_tensor_output_handle of a tensor that does not own its storage alone|holds Core's compute encoder"
        "refused qcore_device_compute_encoder 0|qcore_device_compute_encoder for gpu($GPU_INDEX) was called while package code holds Core's compute encoder|encoder scopes do not nest"
        "refused qcore_device_encode_begin 0|qcore_device_encode_begin for gpu($GPU_INDEX) was called while the thread already holds|package encodes do not nest"
        "refused qcore_device_flush 1|qcore_device_flush was requested on gpu($GPU_INDEX) while package code holds its command stream|Core cannot commit or wait"
        "refused qcore_device_wait 1|a Metal synchronization was requested on gpu($GPU_INDEX) while package code holds its command stream|Core cannot commit or wait"
        "refused qcore_tensor_attach_custom_autograd -6|a custom autograd attach (saving tensors may copy them)|holds Core's compute encoder"
        "the package encoder scope (qcore_device_compute_encoder) on gpu($GPU_INDEX) was still open when the package code returned to Quidra code"
        "refused qcore_tensor_device_handle_const 0|qcore_tensor_device_handle_const of a tensor that is a unified-memory view its source still shares|holds Core's compute encoder"
        "refused qcore_device_flush 1|qcore_device_flush for gpu($other_gpu) was called while the thread holds the command stream of gpu($GPU_INDEX)|a package encode covers one device"
        "refused qcore_device_queue_handle 0|qcore_device_queue_handle for gpu($other_gpu) was called while the thread holds the command stream of gpu($GPU_INDEX)"
        "refused qcore_tensor_device_handle 0|qcore_tensor_device_handle of a tensor that is a view of part of its storage (the borrow first copies it to storage of its own)|holds Core's compute encoder"
    )
    expect_scope_failure() {
        # expect_scope_failure <label> <status> <output> <needles...>: the
        # program stopped (101) with GPU_SCOPE, printed no TENSOR error and
        # nothing after the failure, and its output has every needle.
        local label="$1" status="$2" output="$3" failed=0 needle
        shift 3
        [[ "$status" == "101" ]] || failed=1
        grep -Fq "Quidra runtime error[GPU_SCOPE] at " "$output" || failed=1
        if grep -Fq "Quidra runtime error[TENSOR]" "$output"; then failed=1; fi
        if grep -q "^after" "$output"; then failed=1; fi
        for needle in "$@"; do
            grep -Fq -- "$needle" "$output" || failed=1
        done
        if [[ "$failed" != "0" ]]; then
            echo "$label was not reported as defined (status $status):" >&2
            cat "$output" >&2
            exit 1
        fi
    }
    for kind in 0 1 2 3 4 5 6 7 8 9 10 11 12 13; do
        status="$(run_with_deadline 60 "$TMP/violate.out" env QUIDRA_COUNTERS="$TMP/violate-$kind.jsonl" "$TMP/stream-violate" --kind $kind)"
        IFS='|' read -r -a needles <<< "${violation_expect[$kind]}"
        expect_scope_failure "package encode violation $kind" "$status" "$TMP/violate.out" \
            "before 2.0" "stream-violate.qui:" "${needles[@]}"
    done
    # The refused handles would otherwise need a detach, a copy, a
    # relocation or new storage, whose Core work depends on the upload mode
    # and the pool.
    for kind in 0 3 10 13; do
        for setting in QUIDRA_METAL_UPLOAD=blit QUIDRA_TEST_POOL_POISON=0x5a; do
            status="$(run_with_deadline 60 "$TMP/violate.out" env $setting "$TMP/stream-violate" --kind $kind)"
            IFS='|' read -r -a needles <<< "${violation_expect[$kind]}"
            expect_scope_failure "package encode violation $kind ($setting)" "$status" "$TMP/violate.out" "${needles[@]}"
        done
    done
    python3 - "$TMP"/violate-*.jsonl <<'PY'
import json, sys
for path in sys.argv[1:]:
    total = [json.loads(l) for l in open(path) if '"total"' in l]
    assert total and total[0]["package_scope_violations"] >= 1, (path, total)
PY

    # A package encode left open when the package code returns is ended where
    # control comes back to Quidra code: at the next statement (work 0, each
    # kind), or, when the same statement asks Core for work on the held
    # stream first, at that request, whose refusal names the work: a kernel,
    # a fill, an upload, a same-device copy, a status page clear (works 1-5).
    # Core work in a hold that is only preparing runs; the leak is then
    # reported at the next statement. A leak must not exit 0 with Core's
    # encoder never ended, or make a later Core kernel fail as TENSOR.
    cat > "$TMP/stream-leak.qui" <<QUI
import package = stream_pkg
cli args
    int work = option(default = 0)
    int kind = option(default = 0)
tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * 2.0
tensor<real32> host = tensor.ones<real32>([16]) * 5.0
tensor<int32> ints = tensor.ones<int32>([16], gpu = $GPU_INDEX)
print("before {a[0].item()}")
print(NL)
tensor<real32> r = a
if args.work == 0
    int32 s = package.leak(a, int32(args.kind))
elif args.work == 1
    r = package.leak_kernel(a, int32(args.kind))
elif args.work == 2
    r = package.leak_fill(a, int32(args.kind))
elif args.work == 3
    r = package.leak_upload(a, host, int32(args.kind))
elif args.work == 4
    r = package.leak_transfer(a, int32(args.kind))
else
    r = package.leak_cast(a, ints, int32(args.kind))
print("after {r[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-leak.qui" -o "$TMP/stream-leak" >/dev/null
    leak_name=(
        "the package encoder scope (qcore_device_compute_encoder) on gpu($GPU_INDEX) was still open when the package code returned to Quidra code"
        "the package command-buffer scope (qcore_metal_command_buffer) on gpu($GPU_INDEX) was still open when the package code returned to Quidra code"
        "the package encode hold (qcore_device_encode_begin) on gpu($GPU_INDEX) was still open when the package code returned to Quidra code"
    )
    leak_work=(
        ""
        "a Core compute kernel was requested on gpu($GPU_INDEX)"
        "a Core fill was requested on gpu($GPU_INDEX)"
        "a Core upload was requested on gpu($GPU_INDEX)"
        "a Core device copy was requested on gpu($GPU_INDEX)"
        "a Core status page clear was requested on gpu($GPU_INDEX)"
    )
    for kind in 0 1 2; do
        status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak" --work 0 --kind $kind)"
        expect_scope_failure "package encode left open (kind $kind)" "$status" "$TMP/leak.out" \
            "before 2.0" "${leak_name[$kind]}" "stream-leak.qui:"
    done
    status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak" --work 1 --kind 1)"
    expect_scope_failure "Core work after a command-buffer scope left open" "$status" "$TMP/leak.out" \
        "${leak_work[1]} while package code holds Core's command buffer"
    status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak" --work 1 --kind 2)"
    expect_scope_failure "Core work after a hold left open" "$status" "$TMP/leak.out" \
        "${leak_name[2]}"
    # The blit upload mode would take blit encoders for the fill, the
    # upload and the copy: they are refused before that choice.
    for upload in default blit; do
        for work in 1 2 3 4 5; do
            status="$(run_with_deadline 60 "$TMP/leak.out" env QUIDRA_METAL_UPLOAD=$upload "$TMP/stream-leak" --work $work --kind 0)"
            expect_scope_failure "Core work after a scope left open (work $work, $upload uploads)" "$status" "$TMP/leak.out" \
                "before 2.0" "${leak_work[$work]} while package code holds Core's compute encoder"
        done
    done

    # Left open on a task.all thread by the task's last statement: the end
    # of the task reports it, and nothing waits for the stream for good:
    # neither the main thread's next Core operation nor the stream teardown
    # at exit blocks.
    cat > "$TMP/stream-leak-task.qui" <<QUI
import package = stream_pkg
real64 leaker()
    tensor<real32> x = tensor.ones<real32>([64], gpu = $GPU_INDEX) * 2.0
    return real64(package.leak(x, int32(0)))
real64 idle()
    return 2.0
fn<real64>()[] operations = [leaker, idle]
real64[] results = task.all(operations)
tensor<real32> b = tensor.ones<real32>([64], gpu = $GPU_INDEX) + 1.0
print("after {results[1]} {b[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-leak-task.qui" -o "$TMP/stream-leak-task" >/dev/null
    status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak-task")"
    expect_scope_failure "package encode left open on a task.all thread" "$status" "$TMP/leak.out" \
        "the package encoder scope (qcore_device_compute_encoder) on gpu($GPU_INDEX) was still open when a task.all task returned"

    # A task that breaks its hold (mode 0) or leaves it open (mode 1) while
    # another task waits for the stream: the process ends right after the
    # report with the stream still held, so the other task never runs on
    # into exit. If the hold's command buffer were discarded and the stream
    # released before std::exit, the other task would go on during exit: it
    # would read 1.0 for 3.0, since the work that made its input was in the
    # discarded command buffer, or fail as TENSOR, gpu(0) is not available,
    # while exit tears the device down. A warm-up still running keeps such
    # exit handlers busy (they join its thread), so a task that could run on
    # would.
    cat > "$TMP/stream-task-hold.qui" <<QUI
import package = stream_pkg
cli args
    int mode = option(default = 0)
tensor<real32> a = tensor.ones<real32>([64], gpu = $GPU_INDEX)
print("before {package.warmup_state(a, int32(5))}")
print(NL)
real64 violator()
    tensor<real32> x = tensor.ones<real32>([64], gpu = $GPU_INDEX)
    int32 s = package.task_hold(x, int32(0))
    return 0.0
real64 leaker()
    tensor<real32> x = tensor.ones<real32>([64], gpu = $GPU_INDEX)
    int32 s = package.task_hold(x, int32(1))
    return 0.0
real64 reader()
    tensor<real32> v = tensor.ones<real32>([64], gpu = $GPU_INDEX) * 2.0
    int32 w = package.await_hold()
    int32 z = package.wait_report(v)
    tensor<real32> r = v + 1.0
    print("reader {w} {z} {r[0].item()}")
    print(NL)
    return 0.0
fn<real64>()[] operations = [violator, reader]
if args.mode == 1
    operations = [leaker, reader]
real64[] results = task.all(operations)
print("after")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-task-hold.qui" -o "$TMP/stream-task-hold" >/dev/null
    task_hold_expect=(
        "task flush 1|qcore_device_flush was requested on gpu($GPU_INDEX) while package code holds its command stream"
        "the package encode hold (qcore_device_encode_begin) on gpu($GPU_INDEX) was still open when the package code returned to Quidra code"
    )
    for mode in 0 1; do
        for run in 1 2 3; do
            status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-task-hold" --mode $mode)"
            IFS='|' read -r -a needles <<< "${task_hold_expect[$mode]}"
            expect_scope_failure "a task's hold failure while another task waits (mode $mode, run $run)" \
                "$status" "$TMP/leak.out" "before 0" "${needles[@]}"
            if grep -q "^reader" "$TMP/leak.out" ||
               [[ "$(grep -c "Quidra runtime error" "$TMP/leak.out")" != "1" ]]; then
                echo "another task ran on after a task's hold failure (mode $mode, run $run):" >&2
                cat "$TMP/leak.out" >&2
                exit 1
            fi
        done
    done

    # Left open by a completion callback: reported when the callback
    # returns, naming the root file without a line (no statement runs).
    cat > "$TMP/stream-leak-callback.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * 2.0
int32 armed = package.arm_callback(a, int32(2))
tensor<real32> b = a * 3.0
print("after {armed} {b[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-leak-callback.qui" -o "$TMP/stream-leak-callback" >/dev/null
    status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak-callback")"
    expect_scope_failure "package encode left open by a completion callback" "$status" "$TMP/leak.out" \
        "Quidra runtime error[GPU_SCOPE] at $TMP/stream-leak-callback.qui" "| the package encode hold (qcore_device_encode_begin) on gpu($GPU_INDEX) was still open when a qcore_device_on_complete callback returned"

    # A violation that a completion callback (arm_callback mode 3) or a
    # warm-up ends with qcore_metal_note_work has no statement to stop at
    # either: it names the root file without a line and ends the process at
    # once, the stream
    # still held, so the program never runs on without the work of the
    # discarded command buffer: a warm-up's exit must not unwind out of its
    # own thread's join and let the program print 0.0 for 6.0 and exit 0.
    cat > "$TMP/stream-violate-callback.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * 2.0
int32 armed = package.arm_callback(a, int32(3))
tensor<real32> b = a * 3.0
print("after {armed} {b[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-violate-callback.qui" -o "$TMP/stream-violate-callback" >/dev/null
    status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-violate-callback")"
    expect_scope_failure "a violation in a completion callback" "$status" "$TMP/leak.out" \
        "callback flush 1" \
        "Quidra runtime error[GPU_SCOPE] at $TMP/stream-violate-callback.qui" "| qcore_device_flush was requested on gpu($GPU_INDEX) while package code holds its command stream"
    cat > "$TMP/stream-violate-warmup.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * 2.0
print("before {package.warmup_state(a, int32(3))}")
print(NL)
tensor<real32> b = a * 3.0
time.sleep(time.seconds(10.0))
print("after {b[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-violate-warmup.qui" -o "$TMP/stream-violate-warmup" >/dev/null
    for run in 1 2 3; do
        status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-violate-warmup")"
        expect_scope_failure "a violation in a warm-up (run $run)" "$status" "$TMP/leak.out" \
            "before 0" "warm-up flush 1" \
            "Quidra runtime error[GPU_SCOPE] at $TMP/stream-violate-warmup.qui" "| qcore_device_flush was requested on gpu($GPU_INDEX) while package code holds its command stream"
    done

    # A runtime failure (INDEX_BOUNDS) in the statement whose extern call
    # left a hold or scope open, before the next statement checks it, while a
    # warm-up waits for the held stream: the failure ends the hold (nothing
    # encoded in it runs) and is the failure reported. GPU_SCOPE replaces it
    # only for a violation the hold recorded. Exit then joins the warm-up
    # thread, which gets the stream; joining with the hold in place would
    # hang the process, since the warm-up waits for the stream the exiting
    # thread holds.
    cat > "$TMP/stream-leak-exit.qui" <<QUI
import package = stream_pkg
cli args
    int kind = option(default = 0)
tensor<real32> a = tensor.ones<real32>([64], gpu = $GPU_INDEX) * 2.0
print("before {package.warmup_state(a, int32(4))}")
print(NL)
int[] values = [1, 2, 3]
int v = values[int(package.leak(a, int32(args.kind))) + 10]
print("after {v}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-leak-exit.qui" -o "$TMP/stream-leak-exit" >/dev/null
    for kind in 0 1 2; do
        status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak-exit" --kind $kind)"
        if [[ "$status" != "101" ]] ||
           ! grep -Fq "before 0" "$TMP/leak.out" ||
           ! grep -Fq "Quidra runtime error[INDEX_BOUNDS] at $TMP/stream-leak-exit.qui:8:" "$TMP/leak.out" ||
           grep -Fq "GPU_SCOPE" "$TMP/leak.out" || grep -q "^after" "$TMP/leak.out" ||
           [[ "$(grep -c "Quidra runtime error" "$TMP/leak.out")" != "1" ]]; then
            echo "a failure in a statement that left a hold open (kind $kind) was not reported as defined (status $status):" >&2
            cat "$TMP/leak.out" >&2
            exit 1
        fi
    done

    # A custom autograd backward callback runs inside backward(): a hold or
    # scope it leaves open (kinds 0, 1) and a violation it ends with
    # qcore_metal_note_work (kind 2) are reported at the backward() call,
    # also when a function returned earlier in that statement, not at that
    # function's return statement.
    cat > "$TMP/stream-break-backward.qui" <<QUI
import package = stream_pkg
cli args
    int kind = option(default = 0)
tensor<real32> total(tensor<real32> value)
    tensor<real32> result = value.gather([0], [])
    result = result + value.gather([1], [])
    return result
tensor<real32> x = (tensor.ones<real32>([64], gpu = $GPU_INDEX) * 2.0).track()
tensor<real32> y = package.break_backward(x, int32(args.kind))
print("before")
print(NL)
total(y).backward(&x)
print("after backward")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-break-backward.qui" -o "$TMP/stream-break-backward" >/dev/null
    hold_name=(
        "the package encode hold (qcore_device_encode_begin)"
        "the package encoder scope (qcore_device_compute_encoder)"
    )
    for kind in 0 1 2; do
        status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-break-backward" --kind $kind)"
        if [[ "$kind" == "2" ]]; then
            needles=("backward flush 1"
                "Quidra runtime error[GPU_SCOPE] at $TMP/stream-break-backward.qui:12:1"
                "   | qcore_device_flush was requested on gpu($GPU_INDEX) while package code holds its command stream")
        else
            needles=("Quidra runtime error[GPU_SCOPE] at $TMP/stream-break-backward.qui:12:1"
                     "   | ${hold_name[$kind]} on gpu($GPU_INDEX) was still open when a custom autograd backward callback returned")
        fi
        expect_scope_failure "a custom autograd backward that breaks the protocol (kind $kind)" "$status" "$TMP/leak.out" \
            "before" "${needles[@]}"
        if grep -Fq "node_kind=return" "$TMP/leak.out"; then
            echo "a custom autograd backward's GPU_SCOPE named a returned function's statement:" >&2
            cat "$TMP/leak.out" >&2
            exit 1
        fi
    done

    # A package encode left open earlier in the statement that starts
    # task.all is reported at task.all, before any task starts: the tasks
    # would wait for the held stream while the caller waits for them, and
    # the program would hang.
    cat > "$TMP/stream-leak-task-same.qui" <<QUI
import package = stream_pkg
cli args
    int kind = option(default = 0)
real64 worker()
    tensor<real32> v = tensor.ones<real32>([64], gpu = $GPU_INDEX) * 2.0
    return real64(v[0].item())
fn<real64>()[] operations = [worker, worker]
fn<real64>()[][] sets = [operations]
tensor<real32> x = tensor.ones<real32>([64], gpu = $GPU_INDEX)
real64[] results = task.all(sets[int(package.leak(x, int32(args.kind)))])
print("after {results[0]}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-leak-task-same.qui" -o "$TMP/stream-leak-task-same" >/dev/null
    task_hold_name=(
        "the package encoder scope (qcore_device_compute_encoder)"
        "the package command-buffer scope (qcore_metal_command_buffer)"
        "the package encode hold (qcore_device_encode_begin)"
    )
    for kind in 0 1 2; do
        status="$(run_with_deadline 60 "$TMP/leak.out" "$TMP/stream-leak-task-same" --kind $kind)"
        expect_scope_failure "package encode left open before task.all in its statement (kind $kind)" "$status" "$TMP/leak.out" \
            "Quidra runtime error[GPU_SCOPE] at $TMP/stream-leak-task-same.qui:10:20" \
            "   | ${task_hold_name[$kind]} on gpu($GPU_INDEX) was still open when task.all started its tasks"
    done

    # A mutable CPU pointer of a unified-memory view that device work may
    # still read needs a host wait first, which a hold refuses: always, not
    # only while that work runs, so the result does not depend on timing (a
    # pause before the hold lets the work finish). A const pointer in the
    # hold, and a mutable one taken before it, are served.
    cat > "$TMP/stream-cpu-borrow.qui" <<QUI
import package = stream_pkg
cli args
    int mode = option(default = 0)
    real64 pause = option(default = 0.0)
tensor<real32> twice(tensor<real32> c)
    tensor<real32> g = c.gpu($GPU_INDEX)
    return g * 2.0
tensor<real32> c = tensor.ones<real32>([1048576])
tensor<real32> h = twice(c)
if args.pause > 0.0
    time.sleep(time.seconds(args.pause))
int32 s = package.cpu_borrow(h, &c, int32(args.mode))
print("after {s} {h[0].item()} {c[0].item()}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-cpu-borrow.qui" -o "$TMP/stream-cpu-borrow" >/dev/null
    for pause in 0.0 0.0 0.3; do
        status="$(run_with_deadline 60 "$TMP/borrow.out" "$TMP/stream-cpu-borrow" --mode 0 --pause $pause)"
        expect_scope_failure "a mutable CPU pointer in a hold (pause $pause)" "$status" "$TMP/borrow.out" \
            "pointer 0" "qcore_tensor_cpu_data of a CPU tensor whose unified memory device work may still read" \
            "take it before qcore_device_encode_begin"
    done
    for mode in 1 2; do
        borrow_output="$("$TMP/stream-cpu-borrow" --mode $mode 2>&1 || true)"
        if [[ "$borrow_output" != "$(printf 'pointer 1\nafter 0 2.0 1.0')" ]]; then
            echo "a CPU pointer taken as the protocol allows was refused (mode $mode): '$borrow_output'" >&2
            exit 1
        fi
    done

    # Concurrent package scopes on one device from task.all threads, next to
    # Core work on another thread: every scope holds the stream alone (the
    # others wait), the in-place detach runs in its own hold before its
    # scope, and every value is right.
    cat > "$TMP/stream-concurrent.qui" <<QUI
import package = stream_pkg

real64 inplace_loop()
    int bad = 0
    for i in range(150)
        real32 step = real32(i)
        real32 next_step = real32(i + 1)
        tensor<real32> a = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * step
        tensor<real32> b = package.inplace(a, int32(i % 3))
        if b[4095].item() != next_step or a[0].item() != step
            bad += 1
    return real64(bad)

real64 scale_loop()
    int bad = 0
    for i in range(150)
        real32 step = real32(i)
        real32 doubled_step = real32(2 * i)
        tensor<real32> x = tensor.ones<real32>([4096], gpu = $GPU_INDEX) * step
        tensor<real32> y = package.scale(x, real32(2), int32(i % 5))
        if y[4095].item() != doubled_step
            bad += 1
    return real64(bad)

real64 core_loop()
    tensor<real32> v = tensor.ones<real32>([65536], gpu = $GPU_INDEX)
    for i in range(300)
        v = v + 1.0
    return real64(v[65535].item())

tensor<real32> warm = package.inplace(tensor.ones<real32>([64], gpu = $GPU_INDEX), int32(0))
tensor<real32> warm2 = package.scale(tensor.ones<real32>([64], gpu = $GPU_INDEX), real32(1), int32(4))
fn<real64>()[] operations = [inplace_loop, scale_loop, inplace_loop, scale_loop, core_loop]
real64[] results = task.all(operations)
print("{results[0]} {results[1]} {results[2]} {results[3]} {results[4]}")
print(NL)
QUI
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/stream-concurrent.qui" -o "$TMP/stream-concurrent" >/dev/null
    for upload in default blit; do
        status="$(run_with_deadline 180 "$TMP/concurrent.out" env QUIDRA_METAL_UPLOAD=$upload QUIDRA_BROADCAST=gather "$TMP/stream-concurrent")"
        if [[ "$status" != "0" || "$(cat "$TMP/concurrent.out")" != "0.0 0.0 0.0 0.0 301.0" ]]; then
            echo "concurrent package encoder scopes failed ($upload uploads, status $status):" >&2
            cat "$TMP/concurrent.out" >&2
            exit 1
        fi
    done

    # A failed Core command buffer that a package wait observed (the wait
    # returns nonzero) is still reported at the program's next
    # synchronization point: the wait does not consume the failure, which
    # would otherwise be lost when, for example, a completion callback
    # waited. The failure is injected with a switch of test builds; release
    # builds skip this.
    cat > "$TMP/metal-fail-probe.qui" <<QUI
tensor<real32> x = tensor.ones<real32>([16], gpu = $GPU_INDEX) * 2.0
print(x[0].item())
print(NL)
QUI
    probe_output="$(QUIDRA_TEST_METAL_FAIL_COMMAND=1 "$QUIDRA" run "$TMP/metal-fail-probe.qui" 2>&1 || true)"
    if [[ "$probe_output" == *"test-only Metal command failure"* ]]; then
        cat > "$TMP/stream-wait-error.qui" <<QUI
import package = stream_pkg
tensor<real32> a = tensor.ones<real32>([16], gpu = $GPU_INDEX) * 2.0
print(package.device_wait(a))
print(NL)
tensor<real32> b = a + 1.0
print(b[0].item())
print(NL)
QUI
        set +e
        QUIDRA_TEST_METAL_FAIL_COMMAND=1 QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-wait-error.qui" >"$TMP/wait-error.out" 2>"$TMP/wait-error.err"
        status=$?
        set -e
        if [[ "$status" == "0" || "$(head -n 1 "$TMP/wait-error.out")" != "1" ]] ||
           grep -Fq "3.0" "$TMP/wait-error.out" ||
           ! grep -Fq "test-only Metal command failure" "$TMP/wait-error.err"; then
            echo "a command buffer failure seen by a package wait was not reported at the next host read (status $status):" >&2
            cat "$TMP/wait-error.out" "$TMP/wait-error.err" >&2
            exit 1
        fi
    fi

    # Package warm-up (qcore_register_warmup): a function registered from a
    # static initializer runs once per device, on Core's background thread,
    # only after the program first allocates on that device; one registered
    # later runs at once for the device in use; a repeated registration does
    # not run again.
    cat > "$TMP/stream-warmup.qui" <<QUI
import package = stream_pkg
tensor<real32> host = tensor.ones<real32>([4])
print(package.warmup_state(host, int32(0)))
print(NL)
tensor<real32> a = tensor.ones<real32>([16], gpu = $GPU_INDEX)
print(package.warmup_state(a, int32(1)))
print(NL)
print(package.warmup_state(a, int32(2)))
print(NL)
QUI
    warmup_output="$(QUIDRA_COUNTERS="$TMP/warmup.jsonl" QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/stream-warmup.qui" 2>&1 || true)"
    if [[ "$warmup_output" != "$(printf '0\n1\n1')" ]]; then
        echo "package warm-up functions did not run once per device at its first use:" >&2
        printf '%s\n' "$warmup_output" >&2
        exit 1
    fi
    python3 - "$TMP/warmup.jsonl" <<'PY'
import json, sys
total = [json.loads(l) for l in open(sys.argv[1]) if '"total"' in l][0]
assert total["package_warmups"] == 2, total["package_warmups"]
PY
fi

# A custom native node on Metal receives its gradient contiguous at device
# offset 0 even when the gradient reaches it as a transposed view: callbacks
# take no strides. The probe package's forward and backward are blit
# copies on Core's queue and reject any other layout with status 71. Under
# backward(track = true) its tracked callback runs on the device and attaches
# its second-order node there.
if grep -Fq "backend: Metal" <<<"$gpu_info"; then
    mkdir -p "$TMP/packages/metal_probe/native"
    cat > "$TMP/packages/metal_probe/quidra.package" <<'MANIFEST'
name = metal_probe
version = 0.1.0
native.source.metal = native/metal_probe.mm
MANIFEST
    cat > "$TMP/packages/metal_probe/main.qui" <<'QUI'
extern int32 copy_native(
    const tensor<real32> &input,
    tensor<real32> &output
) = "qtest_metal_copy"

// The identity as a custom native node on the input's Metal device.
tensor<real32> copy(tensor<real32> input)
    tensor<real32> output = tensor.zeros<real32>(input.shape(), gpu = nat(input.device()))
    int32 status = copy_native(&input, &output)
    if status != int32(0)
        error("Metal probe custom autograd registration failed with status {status}")
    return output
QUI
    cat > "$TMP/packages/metal_probe/native/metal_probe.mm" <<'OBJCPP'
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <quidra/native_extension.h>

#include <cstdint>

namespace {

// Callbacks take no strides: only a dense float32 Metal tensor at device
// offset 0 can be read correctly through its buffer handle.
bool dense_metal_f32(const void* tensor) {
    return tensor && qcore_tensor_backend(tensor) == QCORE_BACKEND_METAL &&
           qcore_tensor_dtype(tensor) == QCORE_DTYPE_FLOAT32 &&
           qcore_tensor_is_contiguous(tensor) != 0 &&
           qcore_tensor_device_offset_bytes(tensor) == 0;
}

template <typename T>
T bridge(uint64_t handle) {
    return (__bridge T)(reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)));
}

// Copies source into destination on Core's queue for the source's device
// and waits, so Core sees the result as soon as the call returns.
int blit_copy(const void* source, void* destination) {
    if (!dense_metal_f32(source) || !dense_metal_f32(destination)) return 71;
    const auto count = qcore_tensor_element_count(source);
    if (count == 0 || qcore_tensor_element_count(destination) != count) return 72;
    const auto from = qcore_tensor_device_handle_const(source);
    const auto to = qcore_tensor_device_handle(destination);
    const auto queue = qcore_device_queue_handle(qcore_tensor_device(source));
    if (from == 0 || to == 0 || queue == 0) return 73;
    @autoreleasepool {
        id<MTLCommandBuffer> command = [bridge<id<MTLCommandQueue>>(queue) commandBuffer];
        id<MTLBlitCommandEncoder> blit = command ? [command blitCommandEncoder] : nil;
        if (!blit) return 74;
        [blit copyFromBuffer:bridge<id<MTLBuffer>>(from)
                sourceOffset:0
                    toBuffer:bridge<id<MTLBuffer>>(to)
           destinationOffset:0
                        size:count * sizeof(float)];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        return [command status] == MTLCommandBufferStatusCompleted ? 0 : 75;
    }
}

int copy_backward(
    const void* const*, uint64_t,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*, uint64_t) {
    if (!gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 70;
    return blit_copy(gradient_output, gradient_inputs[0]);
}

// The derivative of the identity: the gradient copy is itself a custom node
// of the incoming gradient, attached on its device.
int copy_backward_tracked(
    const void* const* differentiable_inputs,
    uint64_t differentiable_input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size) {
    if (!differentiable_inputs || differentiable_input_count != 1) return 76;
    const int status = copy_backward(
        saved_tensors, saved_tensor_count, gradient_output,
        gradient_inputs, gradient_input_count, metadata, metadata_size);
    if (status != 0) return status;
    const void* inputs[] = {gradient_output};
    return qcore_tensor_attach_custom_autograd_ex(
        gradient_inputs[0], inputs, 1, copy_backward, nullptr, nullptr, 0);
}

}  // namespace

extern "C" int32_t qtest_metal_copy(const void* input, void* output) {
    const int status = blit_copy(input, output);
    if (status != 0) return status;
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd_ex(
        output, inputs, 1, copy_backward, copy_backward_tracked, nullptr, 0);
}
OBJCPP
    cat > "$TMP/metal-custom-autograd.qui" <<QUI
import probe = metal_probe

tensor<real32> values(int seed)
    tensor<real32> result = tensor.zeros<real32>([2, 3])
    for row in range(2)
        for column in range(3)
            real32 cell = real32((row * 3 + column + seed) % 5)
            result[row, column] = cell + real32(0.5)
    return result

tensor<real32> total(tensor<real32> value)
    tensor<real32> flat = value.reshape([6])
    tensor<real32> result = flat.gather([0], [])
    for index in range(1, 6)
        result = result + flat.gather([index], [])
    return result

// d/dx sum(copy(x)^T * c) = c^T: the gradient reaching the custom node is
// the transposed (strided) view of a dense gradient.
tensor<real32> c = values(2).transpose(0, 1).contiguous().gpu($GPU_INDEX)
tensor<real32> x = values(1).gpu($GPU_INDEX).track()
tensor<real32> y = probe.copy(x)
print((y.untrack().cpu() == values(1)).all())
print(NL)
total(y.transpose(0, 1) * c).backward(&x)
print((x.grad.cpu() == values(2)).all())
print(NL)

// d/du sum(copy(u * u)^T * c) = 2 u c^T, then d/du sum(2 u c^T * p) = 2 c^T p,
// with the first derivative on the device and differentiable.
tensor<real32> u = values(1).gpu($GPU_INDEX).track()
total(probe.copy(u * u).transpose(0, 1) * c).backward(&u, track = true)
tensor<real32> first = u.grad
print(first.device() == $GPU_INDEX and first.is_tracked())
print(NL)
print((first.untrack().cpu() == values(1) * values(2) * real32(2)).all())
print(NL)
u.clear_grad()
total(first * values(3).gpu($GPU_INDEX)).backward(&u)
print((u.grad.cpu() == values(2) * values(3) * real32(2)).all())
print(NL)
QUI
    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/metal-custom-autograd.qui" \
        >"$TMP/metal-custom-autograd.out" 2>"$TMP/metal-custom-autograd.err"
    metal_custom_status=$?
    set -e
    if [[ $metal_custom_status -ne 0 || "$(cat "$TMP/metal-custom-autograd.out")" != "$(printf 'true\ntrue\ntrue\ntrue\ntrue')" ]]; then
        echo "Metal custom native autograd failed on gpu($GPU_INDEX) (status $metal_custom_status)" >&2
        cat "$TMP/metal-custom-autograd.out" >&2 || true
        cat "$TMP/metal-custom-autograd.err" >&2 || true
        exit 1
    fi
fi

# Gradients a masked custom callback declares fully written (full_writes)
# are write-only outputs on Metal: they skip the zero fill, so under
# QUIDRA_TEST_POOL_POISON the callback finds poisoned bytes in them, while
# undeclared gradients still arrive zero-filled. The gradients are bitwise
# those of the same callback without the declaration, and of the CPU.
# QUIDRA_GPU_ZERO_FILL=always restores the fill.
if grep -Fq "backend: Metal" <<<"$gpu_info"; then
    mkdir -p "$TMP/packages/metal_masked/native"
    cat > "$TMP/packages/metal_masked/quidra.package" <<'MANIFEST'
name = metal_masked
version = 0.1.0
native.source.metal = native/metal_masked.mm
MANIFEST
    cat > "$TMP/packages/metal_masked/main.qui" <<'QUI'
extern int32 mul_native(
    const tensor<real32> &left,
    const tensor<real32> &right,
    tensor<real32> &output,
    int64 declared
) = "qtest_metal_mul_masked"
extern int64 unfilled_native() = "qtest_metal_masked_unfilled"
extern int64 unzeroed_native() = "qtest_metal_masked_unzeroed"
extern int64 fills_native() = "qtest_metal_masked_fills"

// left * right as a masked custom node whose callback declares both
// gradients fully written (declared = 1) or neither (declared = 0).
tensor<real32> mul(tensor<real32> left, tensor<real32> right, int declared)
    tensor<real32> output = tensor.zeros<real32>(left.shape(), gpu = nat(left.device()))
    int32 status = mul_native(&left, &right, &output, int64(declared))
    if status != int32(0)
        error("Metal masked custom autograd registration failed with status {status}")
    return output

// Declared gradients that reached the callback with a nonzero byte.
int unfilled()
    return int(unfilled_native())

// Undeclared gradients that reached the callback with a nonzero byte.
int unzeroed()
    return int(unzeroed_native())

// Device zero fills so far (QUIDRA_COUNTERS must be set).
int fills()
    return int(fills_native())
QUI
    cat > "$TMP/packages/metal_masked/native/metal_masked.mm" <<'OBJCPP'
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <quidra/native_extension.h>

#include <cstdint>
#include <cstring>

namespace {

long long unfilled = 0;
long long unzeroed = 0;

bool dense_metal_f32(const void* tensor) {
    return tensor && qcore_tensor_backend(tensor) == QCORE_BACKEND_METAL &&
           qcore_tensor_dtype(tensor) == QCORE_DTYPE_FLOAT32 &&
           qcore_tensor_is_contiguous(tensor) != 0;
}

// Host view of a Metal tensor's shared storage. Valid after every handle is
// taken and qcore_device_wait has returned.
float* host_floats(uint64_t handle, const void* tensor) {
    id<MTLBuffer> buffer =
        (__bridge id<MTLBuffer>)(reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)));
    return reinterpret_cast<float*>(
        static_cast<unsigned char*>([buffer contents]) +
        qcore_tensor_device_offset_bytes(tensor));
}

bool any_nonzero_byte(const float* values, uint64_t count) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(values);
    for (uint64_t index = 0; index < count * sizeof(float); ++index)
        if (bytes[index] != 0) return true;
    return false;
}

// The metadata is the attach-time declaration (one byte per input).
int mul_backward(
    const void* const* saved_tensors, uint64_t saved_tensor_count,
    const void* gradient_output, void* const* gradient_inputs,
    const uint8_t* needed, uint8_t* fully_written,
    uint64_t gradient_input_count, const void* metadata, uint64_t metadata_size) {
    if (!saved_tensors || saved_tensor_count != 2 || !gradient_output ||
        !gradient_inputs || !needed || !fully_written ||
        gradient_input_count != 2 || !metadata || metadata_size != 2)
        return 80;
    const auto* declared = static_cast<const uint8_t*>(metadata);
    if (!dense_metal_f32(saved_tensors[0]) || !dense_metal_f32(saved_tensors[1]) ||
        !dense_metal_f32(gradient_output))
        return 81;
    const auto count = qcore_tensor_element_count(gradient_output);
    const auto x_handle = qcore_tensor_device_handle_const(saved_tensors[0]);
    const auto y_handle = qcore_tensor_device_handle_const(saved_tensors[1]);
    const auto g_handle = qcore_tensor_device_handle_const(gradient_output);
    if (x_handle == 0 || y_handle == 0 || g_handle == 0) return 82;
    uint64_t out_handles[2] = {0, 0};
    for (uint64_t input = 0; input < 2; ++input) {
        if (!needed[input]) continue;
        if (!dense_metal_f32(gradient_inputs[input]) ||
            qcore_tensor_element_count(gradient_inputs[input]) != count)
            return 83;
        out_handles[input] = qcore_tensor_device_handle(gradient_inputs[input]);
        if (out_handles[input] == 0) return 84;
    }
    if (qcore_device_wait(qcore_tensor_device(gradient_output)) != 0) return 85;
    const float* x = host_floats(x_handle, saved_tensors[0]);
    const float* y = host_floats(y_handle, saved_tensors[1]);
    const float* g = host_floats(g_handle, gradient_output);
    for (uint64_t input = 0; input < 2; ++input) {
        if (!needed[input]) continue;
        float* out = host_floats(out_handles[input], gradient_inputs[input]);
        if (any_nonzero_byte(out, count)) {
            if (declared[input]) ++unfilled;
            else ++unzeroed;
        }
        const float* other = input == 0 ? y : x;
        for (uint64_t index = 0; index < count; ++index)
            out[index] = g[index] * other[index];
        fully_written[input] = 1;
    }
    return 0;
}

}  // namespace

extern "C" int32_t qtest_metal_mul_masked(
    const void* left, const void* right, void* output, long long declared) {
    if (!dense_metal_f32(left) || !dense_metal_f32(right) || !dense_metal_f32(output))
        return 90;
    const auto count = qcore_tensor_element_count(left);
    if (qcore_tensor_element_count(right) != count ||
        qcore_tensor_element_count(output) != count)
        return 91;
    const auto a_handle = qcore_tensor_device_handle_const(left);
    const auto b_handle = qcore_tensor_device_handle_const(right);
    const auto out_handle = qcore_tensor_device_handle(output);
    if (a_handle == 0 || b_handle == 0 || out_handle == 0) return 92;
    if (qcore_device_wait(qcore_tensor_device(output)) != 0) return 93;
    const float* a = host_floats(a_handle, left);
    const float* b = host_floats(b_handle, right);
    float* out = host_floats(out_handle, output);
    for (uint64_t index = 0; index < count; ++index) out[index] = a[index] * b[index];
    const void* inputs[] = {left, right};
    const uint8_t flags[] = {
        static_cast<uint8_t>(declared != 0), static_cast<uint8_t>(declared != 0)};
    return qcore_tensor_attach_custom_autograd_masked(
        output, inputs, 2, inputs, 2, mul_backward, nullptr,
        declared != 0 ? flags : nullptr, flags, 2);
}

extern "C" long long qtest_metal_masked_unfilled() { return unfilled; }
extern "C" long long qtest_metal_masked_unzeroed() { return unzeroed; }
extern "C" long long qtest_metal_masked_fills() {
    return static_cast<long long>(qcore_counter_value("fills"));
}
OBJCPP
    cat > "$TMP/metal-masked-fill.qui" <<QUI
import masked = metal_masked
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

tensor<real32> x = xs.gpu($GPU_INDEX)
tensor<real32> y = ys.gpu($GPU_INDEX)
int start = masked.fills()
tensor<real32> declared = masked.mul(x.track(), y.track(), 1) * weights.gpu($GPU_INDEX)
(declared.gather([0], []) + declared.gather([1], []) + declared.gather([2], [])).backward(&x, &y)
int declared_fills = masked.fills() - start

tensor<real32> u = xs.gpu($GPU_INDEX)
tensor<real32> v = ys.gpu($GPU_INDEX)
start = masked.fills()
tensor<real32> plain = masked.mul(u.track(), v.track(), 0) * weights.gpu($GPU_INDEX)
(plain.gather([0], []) + plain.gather([1], []) + plain.gather([2], [])).backward(&u, &v)
int plain_fills = masked.fills() - start

print((x.grad.cpu() == u.grad.cpu()).all() and (y.grad.cpu() == v.grad.cpu()).all())
print(NL)
print((x.grad.cpu() == ys * weights).all() and (y.grad.cpu() == xs * weights).all())
print(NL)
print("{plain_fills - declared_fills} {masked.unfilled()} {masked.unzeroed()}")
print(NL)
QUI
    # One build serves both runs; the fill mode is read at runtime.
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/metal-masked-fill.qui" \
        -o "$TMP/metal-masked-fill" >/dev/null
    for zero_fill in default always; do
        expected="$(printf 'true\ntrue\n2 2 0')"
        fill_env=(env -u QUIDRA_GPU_ZERO_FILL)
        if [[ "$zero_fill" == always ]]; then
            expected="$(printf 'true\ntrue\n0 0 0')"
            fill_env=(env QUIDRA_GPU_ZERO_FILL=always)
        fi
        set +e
        "${fill_env[@]}" QUIDRA_TEST_POOL_POISON=0x5a QUIDRA_COUNTERS="$TMP/metal-masked-$zero_fill.jsonl" \
            "$TMP/metal-masked-fill" >"$TMP/metal-masked.out" 2>"$TMP/metal-masked.err"
        masked_status=$?
        set -e
        if [[ $masked_status -ne 0 || "$(cat "$TMP/metal-masked.out")" != "$expected" ]]; then
            echo "Metal declared full writes ($zero_fill zero fill) on gpu($GPU_INDEX) (status $masked_status)" >&2
            cat "$TMP/metal-masked.out" "$TMP/metal-masked.err" >&2 || true
            exit 1
        fi
    done
fi

echo "real GPU integration: ok on gpu($GPU_INDEX)"
