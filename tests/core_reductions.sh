#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/reductions.qui" <<'QUI'
int[] values := [2, 4, 6]
real average := values.mean()
real variance := values.var()
println(values.sum() == 12 and average == 4.0 and variance == 8.0 / 3)
println(values.min() == 2 and values.max() == 6 and values.std(ddof = 1) == 2.0)
int32[] empty := []
println(empty.sum() == int32(0))
int16[3] fixed := [1, 2, 3]
println(fixed.mean() == 2.0 and fixed.var(ddof = 1) == 1.0)

tensor<real32> data := tensor.zeros<real32>([2, 3])
data[0, 0] = 2.0
data[0, 1] = 4.0
data[0, 2] = 6.0
data[1, 0] = 1.0
data[1, 1] = 3.0
data[1, 2] = 5.0
// Test device transfer is inserted here for the GPU run.
println(data.sum().item() == real32(21) and data.mean().item() == real32(3.5))
println(data.min().item() == real32(1) and data.max().item() == real32(6))
rows := data.std(dim = 1, ddof = 1)
columns := data.transpose(0, 1).max(dim = 1)
println(rows.shape() == [2] and rows.device() == data.device() and rows[0].item() == real32(2) and rows[1].item() == real32(2))
println(columns.shape() == [3] and columns[2].item() == real32(6))
println(data.var(dim = 0, ddof = 1)[1].item() == real32(0.5))

tracked := data.track()
tracked.std(dim = 1, ddof = 1).sum().backward(&tracked)
println(tracked.grad[0, 0].item() == real32(-0.5) and tracked.grad[1, 2].item() == real32(0.5))

constant := (tensor.ones<real32>([2]) * 7.0)
if data.device() >= 0
    constant = constant.gpu(nat(data.device()))
constant = constant.track()
deviation := constant.std()
deviation.backward(&constant)
println(deviation.item() == real32(0) and constant.grad.sum().item() == real32(0))
.constant.clear_grad()
constant.max().backward(&constant)
println(constant.grad[0].item() == real32(1) and constant.grad[1].item() == real32(0))

empty_tensor := tensor.zeros<real32>([0, 3])
if data.device() >= 0
    empty_tensor = empty_tensor.gpu(nat(data.device()))
empty_tensor = empty_tensor.track()
zeros := empty_tensor.sum(dim = 0)
zeros.sum().backward(&empty_tensor)
println(zeros.shape() == [3] and zeros.sum().item() == real32(0) and empty_tensor.grad.shape() == [0, 3])

tensor<nat64> wide := tensor.zeros<nat64>([2])
wide[0] = nat64(9007199254740992)
wide[1] = nat64(9007199254740993)
if data.device() >= 0
    wide = wide.gpu(nat(data.device()))
println(wide.max().item() == nat64(9007199254740993))

tensor<real16> half := tensor.ones<real16>([2])
half[1] = real16(3)
if data.device() >= 0
    half = half.gpu(nat(data.device()))
tensor<real16b> bfloat := real16b(half)
half = half.track()
half_deviation := half.std()
half_deviation.backward(&half)
println(half_deviation.item() == real16(1) and half.grad[0].item() == real16(-0.5) and bfloat.std().item() == real16b(1))
QUI

expected=$(printf 'true\n%.0s' {1..15})
actual=$("$QUIDRA" run "$TMP/reductions.qui")
if [[ "$actual" != "$expected" ]]; then
    printf 'Core reductions output:\n%s\n' "$actual" >&2
    exit 1
fi

if [[ "${2:-OFF}" == ON ]]; then
    sed 's|// Test device transfer is inserted here for the GPU run\.|data = data.gpu(0)|' \
        "$TMP/reductions.qui" > "$TMP/reductions-gpu.qui"
    actual=$(QUIDRA_TEST_FAKE_GPU_COUNT=1 "$QUIDRA" run "$TMP/reductions-gpu.qui")
    if [[ "$actual" != "$expected" ]]; then
        printf 'Core reductions GPU output:\n%s\n' "$actual" >&2
        exit 1
    fi
fi

cat > "$TMP/legacy.qui" <<'QUI'
tensor<T> mean<T: floating>(extend tensor<T> value)
    return value.gather([0], [])
QUI

cat > "$TMP/legacy-reduction.qui" <<'QUI'
import legacy = "./legacy.qui"
data := tensor.ones<real32>([2, 3])
data[0, 0] = real32(7)
println(data.mean().item() == real32(7))
println(data.mean(dim = 1).shape() == [2])
QUI
actual=$("$QUIDRA" run "$TMP/legacy-reduction.qui")
if [[ "$actual" != $'true\ntrue' ]]; then
    printf 'Reduction extension compatibility output:\n%s\n' "$actual" >&2
    exit 1
fi

rejects() {
    local code="$1"
    cat > "$TMP/reject.qui"
    if "$QUIDRA" run "$TMP/reject.qui" > "$TMP/out" 2> "$TMP/err"; then
        printf 'expected %s from invalid reduction\n' "$code" >&2
        exit 1
    fi
    if ! grep -Fq "$code" "$TMP/err"; then
        cat "$TMP/err" >&2
        return 1
    fi
}

rejects REDUCTION_DDOF <<'QUI'
println([2, 4, 6].var(ddof = 3))
QUI

rejects REDUCTION_DDOF <<'QUI'
data := tensor.ones<real64>([2, 3])
println(data.std(dim = 1, ddof = 3).shape().len())
QUI

rejects MATH_SHAPE <<'QUI'
data := tensor.ones<real32>([2, 3])
println(data.min(dim = 2).shape().len())
QUI
