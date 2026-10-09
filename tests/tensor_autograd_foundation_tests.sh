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

cat > "$TMP/tensor-device-metadata.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> value = tensor.ones<real32>([1])
print(value.device() == -1)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/tensor-device-metadata.qui")" == "true" ]]


cat > "$TMP/backward-target-required.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> x = tensor.ones<real32>([]).track()
tensor<real32> loss = x * x
loss.backward()
QUI
set +e
"$QUIDRA" "$TMP/backward-target-required.qui" >"$TMP/backward-target-required.out" 2>"$TMP/backward-target-required.err"
backward_target_required_rc=$?
set -e
if [[ "$backward_target_required_rc" -eq 0 ]]; then
    echo "targetless tensor.backward unexpectedly compiled" >&2
    exit 1
fi
if ! grep -Fq "tensor.backward requires at least one writable gradient target" "$TMP/backward-target-required.err"; then
    cat "$TMP/backward-target-required.err" >&2
    exit 1
fi

cat > "$TMP/backward-const-target.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
const tensor<real32> x = tensor.ones<real32>([]).track()
tensor<real32> loss = x * x
loss.backward(&x)
QUI
set +e
"$QUIDRA" "$TMP/backward-const-target.qui" >"$TMP/backward-const-target.out" 2>"$TMP/backward-const-target.err"
backward_const_target_rc=$?
set -e
if [[ "$backward_const_target_rc" -eq 0 ]]; then
    echo "const tensor.backward target unexpectedly compiled" >&2
    exit 1
fi
if ! grep -Fq "tensor.backward cannot write a gradient through const storage" "$TMP/backward-const-target.err"; then
    cat "$TMP/backward-const-target.err" >&2
    exit 1
fi

cat > "$TMP/backward-target-ampersand-required.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> x = tensor.ones<real32>([]).track()
tensor<real32> loss = x * x
loss.backward(x)
QUI
set +e
"$QUIDRA" "$TMP/backward-target-ampersand-required.qui" >"$TMP/backward-target-ampersand-required.out" 2>"$TMP/backward-target-ampersand-required.err"
backward_ampersand_rc=$?
set -e
if [[ "$backward_ampersand_rc" -eq 0 ]]; then
    echo "tensor.backward target unexpectedly compiled without &" >&2
    exit 1
fi
if ! grep -Fq "tensor.backward gradient targets must be written with &" "$TMP/backward-target-ampersand-required.err"; then
    cat "$TMP/backward-target-ampersand-required.err" >&2
    exit 1
fi

cat > "$TMP/const-tensor-clear-grad.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
const tensor<real32> x = tensor.ones<real32>([]).track()
x.clear_grad()
QUI
set +e
"$QUIDRA" "$TMP/const-tensor-clear-grad.qui" >"$TMP/const-tensor-clear-grad.out" 2>"$TMP/const-tensor-clear-grad.err"
const_tensor_clear_grad_rc=$?
set -e
if [[ "$const_tensor_clear_grad_rc" -eq 0 ]]; then
    echo "const tensor.clear_grad unexpectedly compiled" >&2
    exit 1
fi
if ! grep -Fq "tensor.clear_grad cannot mutate through a const access path" "$TMP/const-tensor-clear-grad.err"; then
    cat "$TMP/const-tensor-clear-grad.err" >&2
    exit 1
fi

cat > "$TMP/fixed-array-backward-targets.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
class Leaf
    autograd.Target target

    construct()
        this.target = autograd.target()

class Model
    Leaf[2] leaves

    construct()
        this.leaves = [Leaf(), Leaf()]

Model model = Model()
tensor<real32> left = tensor.ones<real32>([]).track(
    &model.leaves[0].target
)
tensor<real32> right = tensor.ones<real32>([]).track(
    &model.leaves[1].target
)
tensor<real32> loss = left * right
loss.backward(&model)
print(model.leaves[0].target.has_grad())
print(NL)
print(model.leaves[1].target.has_grad())
print(NL)
QUI
fixed_array_backward_output="$("$QUIDRA" "$TMP/fixed-array-backward-targets.qui")"
fixed_array_backward_expected="$(printf 'true\ntrue')"
if [[ "$fixed_array_backward_output" != "$fixed_array_backward_expected" ]]; then
    echo "unexpected fixed-array backward target output:" >&2
    printf '%s\n' "$fixed_array_backward_output" >&2
    exit 1
fi

cat > "$TMP/runtime-array-backward-targets.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
class Leaf
    autograd.Target target

    construct()
        this.target = autograd.target()

class Model
    Leaf[] leaves

    construct()
        this.leaves = []
        this.leaves = this.leaves.append(Leaf())
        this.leaves = this.leaves.append(Leaf())

Model model = Model()
tensor<real32> left = tensor.ones<real32>([]).track(
    &model.leaves[0].target
)
tensor<real32> right = tensor.ones<real32>([]).track(
    &model.leaves[1].target
)
tensor<real32> loss = left * right
loss.backward(&model)
print(model.leaves[0].target.has_grad())
print(NL)
print(model.leaves[1].target.has_grad())
print(NL)
QUI
runtime_array_backward_output="$("$QUIDRA" "$TMP/runtime-array-backward-targets.qui")"
runtime_array_backward_expected="$(printf 'true\ntrue')"
if [[ "$runtime_array_backward_output" != "$runtime_array_backward_expected" ]]; then
    echo "unexpected runtime-array backward target output:" >&2
    printf '%s\n' "$runtime_array_backward_output" >&2
    exit 1
fi

cat > "$TMP/broadcast-autograd.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> left_values = tensor.zeros<real32>([2, 1])
left_values[0, 0] = real32(2)
left_values[1, 0] = real32(3)
tensor<real32> left = left_values.track()
tensor<real32> right = tensor.ones<real32>([2, 3]).track()
tensor<real32> output = left * right
print(output.shape()[0] == 2 and output.shape()[1] == 3)
print(NL)
reductions.mean(output).backward(&left, &right, track = true)
print(left.grad.is_tracked())
print(NL)
print(right.grad.is_tracked())
print(NL)
tensor<real32> left_grad = left.grad.untrack()
tensor<real32> right_grad = right.grad.untrack()
print(left_grad[0, 0].item() == real32(0.5))
print(NL)
print(left_grad[1, 0].item() == real32(0.5))
print(NL)
print(right_grad[0, 0].item() > real32(0.3333) and right_grad[0, 0].item() < real32(0.3334))
print(NL)
print(right_grad[1, 2].item() == real32(0.5))
print(NL)
reductions.mean(left.grad).backward(&right)
tensor<real32> accumulated = right.grad.untrack()
print(accumulated[0, 0].item() > real32(0.4166) and accumulated[0, 0].item() < real32(0.4168))
print(NL)
print(accumulated[1, 2].item() > real32(0.5832) and accumulated[1, 2].item() < real32(0.5834))
print(NL)
QUI

broadcast_autograd_output="$("$QUIDRA" "$TMP/broadcast-autograd.qui")"
broadcast_autograd_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$broadcast_autograd_output" != "$broadcast_autograd_expected" ]]; then
    echo "unexpected tensor broadcast autograd output:" >&2
    printf '%s\n' "$broadcast_autograd_output" >&2
    exit 1
fi

cat > "$TMP/autograd-contracts.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> weight = tensor.ones<real32>([1, 2])
tensor<real32> bias = tensor.zeros<real32>([1])
tensor<real32> samples = tensor.ones<real32>([1, 2])
tensor<real32> tracked = samples.track()
tensor<real32> tracked_weight = weight.track()
tensor<real32> tracked_bias = bias.track()
tensor<real32> prediction = tracked * tracked_weight
prediction = prediction + tracked_bias.gather([0, 0], [1, 2])
reductions.mean(prediction).backward(&tracked, &weight, &bias)

print(tracked.grad[0, 0].item())
print(NL)
print(tracked.grad[0, 1].item())
print(NL)
print(weight.grad[0, 0].item())
print(NL)
print(weight.grad[0, 1].item())
print(NL)
print(bias.grad[0].item())
print(NL)

reductions.mean(prediction).backward(&tracked, &weight, &bias)
print(weight.grad[0, 0].item())
print(NL)

tensor<real32> tracked_value = tensor.ones<real32>([]).track()
tensor<real32> constant_value = tensor.ones<real32>([]) * real32(4)
tensor<real32> mixed = tracked_value * constant_value
mixed.backward(&tracked_value)
print(tracked_value.grad.item())
print(NL)
QUI

autograd_contract_output="$("$QUIDRA" "$TMP/autograd-contracts.qui")"
autograd_contract_expected="$(printf '0.5\n0.5\n0.5\n0.5\n1.0\n1.0\n4.0')"
if [[ "$autograd_contract_output" != "$autograd_contract_expected" ]]; then
    echo "unexpected tensor autograd contract output:" >&2
    printf '%s\n' "$autograd_contract_output" >&2
    exit 1
fi

cat > "$TMP/untracked-forward.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> weight = tensor.ones<real32>([1, 2])
tensor<real32> bias = tensor.zeros<real32>([1])
tensor<real32> samples = tensor.ones<real32>([1, 2])
tensor<real32> prediction = samples * weight
prediction = prediction + bias.gather([0, 0], [1, 2])
reductions.mean(prediction).backward(&samples)
QUI
set +e
"$QUIDRA" "$TMP/untracked-forward.qui" >"$TMP/untracked-forward.out" 2>"$TMP/untracked-forward.err"
untracked_forward_rc=$?
set -e
if [[ "$untracked_forward_rc" -ne 101 ]]; then
    echo "untracked tensor forward unexpectedly created an autograd graph" >&2
    exit 1
fi
if ! grep -Fq "backward() requires a tracked tensor" "$TMP/untracked-forward.err"; then
    cat "$TMP/untracked-forward.err" >&2
    exit 1
fi

cat > "$TMP/untracked-leaf-grad.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([1])
print(source.grad[0].item())
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/untracked-leaf-grad.qui" >"$TMP/untracked-leaf-grad.out" 2>"$TMP/untracked-leaf-grad.err"
untracked_leaf_rc=$?
set -e
if [[ "$untracked_leaf_rc" -ne 101 ]]; then
    echo "untracked tensor unexpectedly acquired a gradient" >&2
    exit 1
fi
if ! grep -Fq "tensor gradient is not available" "$TMP/untracked-leaf-grad.err"; then
    cat "$TMP/untracked-leaf-grad.err" >&2
    exit 1
fi

cat > "$TMP/retrack-cut.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> root = tensor.ones<real32>([]).track()
tensor<real32> before_cut = root * real32(2)
tensor<real32> after_cut = before_cut.retrack()
tensor<real32> loss = after_cut * real32(3)
loss.backward(&after_cut, &root)
print(after_cut.grad.item())
print(NL)
print(root.grad.item())
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/retrack-cut.qui" >"$TMP/retrack-cut.out" 2>"$TMP/retrack-cut.err"
retrack_cut_rc=$?
set -e
if [[ "$retrack_cut_rc" -ne 101 ]]; then
    echo "retrack() did not cut the previous graph" >&2
    exit 1
fi
if [[ "$(cat "$TMP/retrack-cut.out")" != "3.0" ]]; then
    echo "unexpected retrack root gradient" >&2
    cat "$TMP/retrack-cut.out" >&2
    exit 1
fi
if ! grep -Fq "tensor gradient is not available" "$TMP/retrack-cut.err"; then
    cat "$TMP/retrack-cut.err" >&2
    exit 1
fi

cat > "$TMP/tensor-grad-state.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> root = tensor.ones<real32>([]).track()
tensor<real32> loss = root * real32(2)
print(not root.has_grad())
print(NL)
loss.backward(&root)
print(root.has_grad())
print(NL)
print(root.grad.untrack().item() == real32(2))
print(NL)
root.clear_grad()
print(not root.has_grad())
print(NL)
loss.backward(&root)
print(root.has_grad())
print(NL)
print(root.grad.untrack().item() == real32(2))
print(NL)
QUI
tensor_grad_state_output="$("$QUIDRA" "$TMP/tensor-grad-state.qui")"
tensor_grad_state_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tensor_grad_state_output" != "$tensor_grad_state_expected" ]]; then
    echo "unexpected tensor gradient state output:" >&2
    printf '%s\n' "$tensor_grad_state_output" >&2
    exit 1
fi

cat > "$TMP/explicit-gradient-selection-and-accumulation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> x = tensor.ones<real32>([]).track()
tensor<real32> y_value = tensor.ones<real32>([]) * real32(3)
tensor<real32> y = y_value.track()
tensor<real32> loss = x * y

loss.backward(&x)
print(x.grad.item() == real32(3))
print(NL)
print(not y.has_grad())
print(NL)

loss.backward(&x)
print(x.grad.item() == real32(6))
print(NL)
print(not y.has_grad())
print(NL)

x.clear_grad()
loss.backward(&x, &y)
print(x.grad.item() == real32(3))
print(NL)
print(y.grad.item() == real32(1))
print(NL)
QUI
selection_accumulation_output="$("$QUIDRA" run "$TMP/explicit-gradient-selection-and-accumulation.qui")"
selection_accumulation_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$selection_accumulation_output" != "$selection_accumulation_expected" ]]; then
    echo "unexpected explicit gradient selection/accumulation output:" >&2
    printf '%s\n' "$selection_accumulation_output" >&2
    exit 1
fi

cat > "$TMP/untrack-cut.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> root = tensor.ones<real32>([]).track()
tensor<real32> detached = (root * real32(2)).untrack()
tensor<real32> loss = detached * real32(3)
loss.backward(&root)
QUI
set +e
"$QUIDRA" "$TMP/untrack-cut.qui" >"$TMP/untrack-cut.out" 2>"$TMP/untrack-cut.err"
untrack_cut_rc=$?
set -e
if [[ "$untrack_cut_rc" -ne 101 ]]; then
    echo "untrack() did not disconnect the graph" >&2
    exit 1
fi
if ! grep -Fq "backward() requires a tracked tensor" "$TMP/untrack-cut.err"; then
    cat "$TMP/untrack-cut.err" >&2
    exit 1
fi

cat > "$TMP/untracked-constant-grad.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> tracked = tensor.ones<real32>([]).track()
tensor<real32> constant = tensor.ones<real32>([]) * real32(4)
tensor<real32> loss = tracked * constant
loss.backward(&tracked, &constant)
QUI
set +e
"$QUIDRA" "$TMP/untracked-constant-grad.qui" >"$TMP/untracked-constant-grad.out" 2>"$TMP/untracked-constant-grad.err"
untracked_constant_rc=$?
set -e
if [[ "$untracked_constant_rc" -ne 101 ]]; then
    echo "untracked tensor unexpectedly accepted as an explicit gradient target" >&2
    exit 1
fi
if ! grep -Fq "gradient tensor target is not tracked" "$TMP/untracked-constant-grad.err"; then
    cat "$TMP/untracked-constant-grad.err" >&2
    exit 1
fi

cat > "$TMP/tracked-mutation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> tracked = tensor.ones<real32>([2]).track()
tracked[0] = real32(2)
QUI
set +e
"$QUIDRA" "$TMP/tracked-mutation.qui" >"$TMP/tracked-mutation.out" 2>"$TMP/tracked-mutation.err"
tracked_mutation_rc=$?
set -e
if [[ "$tracked_mutation_rc" -ne 101 ]]; then
    echo "tracked tensor mutation unexpectedly succeeded" >&2
    exit 1
fi
if ! grep -Fq "tracked tensor mutation is forbidden; call untrack() before writing" "$TMP/tracked-mutation.err"; then
    cat "$TMP/tracked-mutation.err" >&2
    exit 1
fi

for transform in contiguous indexing; do
    case "$transform" in
        contiguous)
            body='tensor<real32> ignored = tracked.contiguous()'
            ;;
        indexing)
            body='tensor<real32> ignored = tracked[0:1, 0:2]'
            ;;
    esac
    cat > "$TMP/tracked-transform.qui" <<QUI
tensor<real32> tracked = tensor.ones<real32>([2, 2]).track()
$body
QUI
    set +e
    "$QUIDRA" "$TMP/tracked-transform.qui" >"$TMP/tracked-transform.out" 2>"$TMP/tracked-transform.err"
    tracked_transform_rc=$?
    set -e
    if [[ "$tracked_transform_rc" -ne 101 ]]; then
        echo "tracked tensor $transform unexpectedly bypassed the explicit-untrack boundary" >&2
        exit 1
    fi
    if ! grep -Fq "requires explicit untrack() first" "$TMP/tracked-transform.err"; then
        cat "$TMP/tracked-transform.err" >&2
        exit 1
    fi
done

cat > "$TMP/tensor-power-autograd.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"

tensor<real32> values = tensor.ones<real32>([1, 3]).track()
tensor<real32> squared = values ^ real32(2)
tensor<real32> total = reductions.sum(squared)
total.backward(&values, track = true)
tensor<real32> first = values.grad
print(first.is_tracked())
print(NL)
print(first.untrack()[0, 2].item() == real32(2))
print(NL)
values.clear_grad()
reductions.sum(first).backward(&values)
print(values.grad.untrack()[0, 0].item() == real32(2))
print(NL)
QUI

power_autograd_output="$("$QUIDRA" "$TMP/tensor-power-autograd.qui")"
power_autograd_expected="$(printf 'true\ntrue\ntrue')"
if [[ "$power_autograd_output" != "$power_autograd_expected" ]]; then
    echo "unexpected tensor power/autograd output: $power_autograd_output" >&2
    exit 1
fi

cat > "$TMP/tensor-sum.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> values = tensor.ones<real32>([2, 3]).track()
tensor<real32> total = reductions.sum(values)
print(total.untrack().item() == real32(6))
print(NL)
(total * total).backward(&values, track = true)
tensor<real32> first = values.grad
print(first.is_tracked())
print(NL)
print(first.untrack()[0, 0].item() == real32(12))
print(NL)
values.clear_grad()
reductions.sum(first).backward(&values)
print(values.grad.untrack()[1, 2].item() == real32(12))
print(NL)
tensor<real32> empty = tensor.zeros<real32>([0])
print(reductions.sum(empty).item() == real32(0))
print(NL)
QUI
sum_output="$("$QUIDRA" "$TMP/tensor-sum.qui")"
sum_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue')"
if [[ "$sum_output" != "$sum_expected" ]]; then
    echo "unexpected tensor sum output: $sum_output" >&2
    exit 1
fi

for removed_reduction in sum mean sum_last max_last min_last; do
    cat > "$TMP/removed-reduction.qui" <<QUI
tensor<real32> value = tensor.ones<real32>([1, 2])
tensor<real32> invalid = value.${removed_reduction}()
QUI
    set +e
    "$QUIDRA" check "$TMP/removed-reduction.qui" >"$TMP/removed-reduction.out" 2>"$TMP/removed-reduction.err"
    removed_reduction_status=$?
    set -e
    if [[ "$removed_reduction_status" -eq 0 ]]; then
        echo "removed Core tensor reduction method unexpectedly compiled: $removed_reduction" >&2
        exit 1
    fi
done

cat > "$TMP/tensor-grad-copy-isolation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([2])
tensor<real32> alias = source
tensor<real32> tracked = source.track()
reductions.mean((tracked * tracked)).backward(&source)
print(source.has_grad())
print(NL)
print(not alias.has_grad())
print(NL)
tensor<real32> gradient = source.grad
print(gradient[0].item() == real32(1))
print(NL)
print(gradient[1].item() == real32(1))
print(NL)
source.clear_grad()
print(not source.has_grad())
print(NL)
print(not alias.has_grad())
print(NL)
QUI
tensor_grad_slot_output="$("$QUIDRA" run "$TMP/tensor-grad-copy-isolation.qui")"
tensor_grad_slot_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tensor_grad_slot_output" != "$tensor_grad_slot_expected" ]]; then
    echo "unexpected tensor grad-slot output:" >&2
    printf '%s\n' "$tensor_grad_slot_output" >&2
    exit 1
fi

cat > "$TMP/tracked-copy-gradient-identity.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([1]).track()
tensor<real32> copy = source
tensor<real32> source_loss = source * real32(2)
source_loss.backward(&source)
print(source.has_grad())
print(NL)
print(not copy.has_grad())
print(NL)

tensor<real32> copy_loss = copy * real32(3)
copy_loss.backward(&copy)
print(copy.has_grad())
print(NL)
print(copy.grad[0].item() == real32(3))
print(NL)
print(source.grad[0].item() == real32(2))
print(NL)

source.clear_grad()
copy.clear_grad()
source_loss.backward(&source, &copy)
print(source.grad[0].item() == real32(2))
print(NL)
print(copy.grad[0].item() == real32(2))
print(NL)

source.clear_grad()
source_loss.backward(&source, &source)
print(source.grad[0].item() == real32(2))
print(NL)

tensor<real32> explicit_track = source.track()
tensor<real32> explicit_loss = explicit_track * real32(4)
explicit_loss.backward(&explicit_track)
print(explicit_track.has_grad())
print(NL)
print(explicit_track.grad[0].item() == real32(4))
print(NL)
print(source.grad[0].item() == real32(2))
print(NL)
QUI
tracked_copy_output="$("$QUIDRA" run "$TMP/tracked-copy-gradient-identity.qui")"
tracked_copy_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tracked_copy_output" != "$tracked_copy_expected" ]]; then
    echo "unexpected tracked tensor copy gradient identity output:" >&2
    printf '%s\n' "$tracked_copy_output" >&2
    exit 1
fi

cat > "$TMP/tracked-dtype-cast.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([2])
tensor<real32> tracked = source.track()
tensor<real64> invalid = real64(tracked)
print(invalid.shape()[0])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/tracked-dtype-cast.qui" >"$TMP/tracked-dtype-cast.out" 2>"$TMP/tracked-dtype-cast.err"
tracked_cast_status=$?
set -e
if [[ $tracked_cast_status -ne 101 ]]; then
    echo "tracked tensor dtype cast unexpectedly succeeded" >&2
    exit 1
fi
if ! grep -Fq "tracked tensor cannot be cast; call untrack() explicitly before changing dtype" "$TMP/tracked-dtype-cast.err"; then
    echo "missing tracked tensor cast diagnostic" >&2
    cat "$TMP/tracked-dtype-cast.err" >&2
    exit 1
fi

cat > "$TMP/untracked-dtype-cast.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([2])
tensor<real32> tracked = source.track()
tensor<real64> converted = real64(tracked.untrack())
print(converted.shape()[0])
print(NL)
print(converted[0].item())
print(NL)
QUI
untracked_cast_output="$("$QUIDRA" "$TMP/untracked-dtype-cast.qui")"
if [[ "$untracked_cast_output" != "$(printf '2\n1.0')" ]]; then
    echo "unexpected explicit-untrack dtype cast output: $untracked_cast_output" >&2
    exit 1
fi

cat > "$TMP/value-copy-autograd.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> tensor_original = tensor.ones<real32>([2])
tensor<real32> tensor_copy = tensor_original
print(&tensor_original != &tensor_copy)
print(NL)
tensor_copy[0] = 9.0
print(tensor_original[0].item())
print(NL)
print(tensor_copy[0].item())
print(NL)
QUI

value_copy_output="$("$QUIDRA" "$TMP/value-copy-autograd.qui")"
value_copy_expected="$(printf 'true\n1.0\n9.0')"
if [[ "$value_copy_output" != "$value_copy_expected" ]]; then
    echo "unexpected value-copy/autograd output:" >&2
    printf '%s\n' "$value_copy_output" >&2
    exit 1
fi

cat > "$TMP/primitive-inference.qui" <<'QUI'
X identity<X>(X value)
    return value

tensor<real32> values = tensor.ones<real32>([1, 3])
tensor<real32> tracked = values.track()
print(identity(tracked ^ real32(2)).untrack().shape()[1])
print(NL)
QUI

inference_output="$("$QUIDRA" "$TMP/primitive-inference.qui")"
if [[ "$inference_output" != "3" ]]; then
    echo "unexpected tensor primitive inference output: $inference_output" >&2
    exit 1
fi

cat > "$TMP/tracked-mutation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> value = tensor.ones<real32>([2]).track()
value[0] = real32(3)
QUI
set +e
"$QUIDRA" "$TMP/tracked-mutation.qui" >"$TMP/tracked-mutation.out" 2>"$TMP/tracked-mutation.err"
tracked_mutation_rc=$?
set -e
if [[ "$tracked_mutation_rc" -ne 101 ]]; then
    echo "tracked tensor mutation unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "tracked tensor mutation is forbidden; call untrack() before writing" "$TMP/tracked-mutation.err"

cat > "$TMP/tracked-alias-mutation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> value = tensor.ones<real32>([2]).track()
tensor<real32> alias = value
alias[0] = real32(3)
QUI
set +e
"$QUIDRA" "$TMP/tracked-alias-mutation.qui" >/dev/null 2>"$TMP/tracked-alias-mutation.err"
tracked_alias_rc=$?
set -e
if [[ "$tracked_alias_rc" -ne 101 ]]; then
    echo "tracked tensor alias mutation unexpectedly succeeded" >&2
    exit 1
fi
if ! grep -Fq "tracked tensor mutation is forbidden; call untrack() before writing" "$TMP/tracked-alias-mutation.err"; then
    cat "$TMP/tracked-alias-mutation.err" >&2
    exit 1
fi

for transform in contiguous index; do
    case "$transform" in
        contiguous)
            body='tensor<real32> changed = value.contiguous()'
            expected='contiguous() on a tracked tensor requires explicit untrack() first'
            ;;
        index)
            body='tensor<real32> changed = value[0]'
            expected='indexing on a tracked tensor requires explicit untrack() first'
            ;;
    esac
    cat > "$TMP/tracked-transform.qui" <<QUI
tensor<real32> value = tensor.ones<real32>([1, 2]).track()
$body
print(changed.shape()[0])
print(NL)
QUI
    set +e
    "$QUIDRA" "$TMP/tracked-transform.qui" >/dev/null 2>"$TMP/tracked-transform.err"
    transform_rc=$?
    set -e
    if [[ "$transform_rc" -ne 101 ]]; then
        echo "tracked tensor $transform unexpectedly detached" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$TMP/tracked-transform.err"; then
        cat "$TMP/tracked-transform.err" >&2
        exit 1
    fi
done

cat > "$TMP/untracked-mutation.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> value = tensor.ones<real32>([2]).track().untrack()
value[0] = real32(3)
print(value[0].item())
print(NL)
QUI
if [[ "$("$QUIDRA" "$TMP/untracked-mutation.qui")" != "3.0" ]]; then
    echo "explicitly untracked tensor mutation failed" >&2
    exit 1
fi

cat > "$TMP/higher-order.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> x = tensor.ones<real32>([]).track()
tensor<real32> loss = x * x * x
loss.backward(&x, track = true)
print(x.grad.item())
print(NL)
tensor<real32> first = x.grad
first.backward(&x, track = true)
print(x.grad.item())
print(NL)
tensor<real32> second = x.grad
second.backward(&x)
print(x.grad.item())
print(NL)
QUI

higher_order_output="$("$QUIDRA" "$TMP/higher-order.qui")"
higher_order_expected="$(printf '3.0\n9.0\n21.0')"
if [[ "$higher_order_output" != "$higher_order_expected" ]]; then
    echo "unexpected higher-order tensor autograd output:" >&2
    printf '%s\n' "$higher_order_output" >&2
    exit 1
fi

# Autograd regressions, kept in one program so that this
# script compiles them once.
# Tracked negation is recorded in the graph instead of silently
# cutting it: d/dx (x * -x) = -2x, and the symbolic engine differentiates it
# again.
# The higher-order seed stays exactly one for a non-finite loss.
# track() and clear_grad() on a by-value tensor parameter never
# change the caller's tensor.
cat > "$TMP/autograd-defects.qui" <<'QUI'
autograd.Target negation_target = autograd.target()
tensor<real32> negation_x = tensor.ones<real32>([]).track(&negation_target)
tensor<real32> negated = -negation_x
print(negated.is_tracked())
print(NL)
(negation_x * negated).backward(&negation_target)
print(negation_target.gradient<real32>().item())
print(NL)
tensor<real32> cube = (tensor.ones<real32>([]) * real32(2)).track()
(-(cube * cube * cube)).backward(&cube, track = true)
tensor<real32> cube_first = cube.grad
print(cube_first.untrack().item())
print(NL)
cube.clear_grad()
(-cube_first).backward(&cube)
print(cube.grad.item())
print(NL)
tensor<real64> pair = (tensor.ones<real64>([2]) * 3.0).track()
(-pair).reshape([2]).gather([1], []).backward(&pair)
print(pair.grad[0].item() == 0.0 and pair.grad[1].item() == -1.0)
print(NL)
// The higher-order seed is exactly one even when the loss is not
// finite (it used to be loss * 0 + 1 = NaN), and a leaf that the loss
// reaches only through the seed still receives its zero second derivative.
autograd.Target seed_target = autograd.target()
tensor<real32> seed_x = (tensor.ones<real32>([]) * real32(2)).track(&seed_target)
tensor<real32> huge = tensor.ones<real32>([]) * real32(300000000000000000000000000000000000000.0)
tensor<real32> overflow = seed_x * huge
print(overflow.untrack().cpu().item())
print(NL)
overflow.backward(&seed_target, track = true)
print(seed_target.gradient<real32>().untrack().cpu().item())
print(NL)
tensor<real32> linear = (tensor.ones<real32>([]) * real32(5)).track()
(linear * real32(3)).backward(&linear, track = true)
tensor<real32> slope = linear.grad
linear.clear_grad()
slope.backward(&linear)
print(linear.grad.cpu().item())
print(NL)
// A by-value tensor parameter has its own autograd state, whether
// or not the compiler passes it without a clone: track() and clear_grad()
// inside the callee must not change the caller's tensor (the owned
// variants, which rebind the parameter first, always behaved this way).
void wipe(tensor<real32> value)
    value.clear_grad()

void wipe_owned(tensor<real32> value)
    value.clear_grad()
    value = value

tensor<real32> start(tensor<real32> value)
    return value.track()

tensor<real32> start_owned(tensor<real32> value)
    value = value
    return value.track()

tensor<real32> cleared = tensor.ones<real32>([1]).track()
(cleared * 3.0).reshape([]).backward(&cleared)
wipe(cleared)
print(cleared.has_grad())
print(NL)
wipe_owned(cleared)
print(cleared.has_grad())
print(NL)
tensor<real32> source = tensor.ones<real32>([1])
tensor<real32> started = start(source)
tensor<real32> tracked_source = source.track()
(started * 2.0 + tracked_source * 5.0).reshape([]).backward(&started)
print(started.grad[0].item())
print(NL)
tensor<real32> owned_source = tensor.ones<real32>([1])
tensor<real32> owned_started = start_owned(owned_source)
tensor<real32> owned_tracked = owned_source.track()
(owned_started * 2.0 + owned_tracked * 5.0).reshape([]).backward(&owned_started)
print(owned_started.grad[0].item())
print(NL)
// The same rule for a match binder borrowed from the variant's payload.
tensor<real32> | error choice = tensor.ones<real32>([1])
tensor<real32> matched = tensor.zeros<real32>([1])
tensor<real32> matched_again = tensor.zeros<real32>([1])
match choice
    tensor<real32> payload
        matched = payload.track()
    error problem
        print(problem)
match choice
    tensor<real32> payload
        matched_again = payload.track()
    error problem
        print(problem)
(matched * 2.0 + matched_again * 5.0).reshape([]).backward(&matched)
print(matched.grad[0].item())
print(NL)
QUI
autograd_defects_output="$("$QUIDRA" "$TMP/autograd-defects.qui")"
autograd_defects_expected="$(printf 'true\n-2.0\n-12.0\n12.0\ntrue\ninf\n3.0000000054977558e+38\n0.0\ntrue\ntrue\n2.0\n2.0\n2.0')"
if [[ "$autograd_defects_output" != "$autograd_defects_expected" ]]; then
    echo "unexpected autograd defect regression output:" >&2
    printf '%s\n' "$autograd_defects_output" >&2
    echo "expected:" >&2
    printf '%s\n' "$autograd_defects_expected" >&2
    exit 1
fi

cat > "$TMP/tensor-view-autograd.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source_values = tensor.zeros<real32>([2, 3])
source_values[0, 0] = real32(1)
source_values[0, 1] = real32(2)
source_values[0, 2] = real32(3)
source_values[1, 0] = real32(4)
source_values[1, 1] = real32(5)
source_values[1, 2] = real32(6)
tensor<real32> source = source_values.track()
tensor<real32> transposed = source.transpose(0, 1)
print(transposed.is_tracked())
print(NL)
print(transposed.shape()[0] == 3 and transposed.shape()[1] == 2)
print(NL)
print(transposed.untrack()[0, 1].item() == real32(4))
print(NL)
tensor<real32> loss = reductions.mean((transposed * transposed))
loss.backward(&source, track = true)
tensor<real32> first = source.grad
print(first.is_tracked())
print(NL)
tensor<real32> first_values = first.untrack()
print(first_values[0, 0].item() > real32(0.33) and first_values[0, 0].item() < real32(0.34))
print(NL)
print(first_values[1, 2].item() == real32(2))
print(NL)
reductions.mean(first).backward(&source)
tensor<real32> accumulated = source.grad.untrack()
print(accumulated[0, 0].item() > real32(0.38) and accumulated[0, 0].item() < real32(0.40))
print(NL)
print(accumulated[1, 2].item() > real32(2.05) and accumulated[1, 2].item() < real32(2.06))
print(NL)

tensor<real32> reshape_source = tensor.ones<real32>([2, 3]).track()
tensor<real32> reshaped = reshape_source.reshape([3, 2])
print(reshaped.is_tracked())
print(NL)
reductions.mean((reshaped * reshaped)).backward(&reshape_source, track = true)
tensor<real32> reshape_first = reshape_source.grad
print(reshape_first.is_tracked())
print(NL)
tensor<real32> reshape_values = reshape_first.untrack()
print(reshape_values[0, 0].item() > real32(0.33) and reshape_values[0, 0].item() < real32(0.34))
print(NL)
reductions.mean(reshape_first).backward(&reshape_source)
tensor<real32> reshape_accumulated = reshape_source.grad.untrack()
print(reshape_accumulated[0, 0].item() > real32(0.38) and reshape_accumulated[0, 0].item() < real32(0.40))
print(NL)
QUI
view_output="$("$QUIDRA" "$TMP/tensor-view-autograd.qui")"
view_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$view_output" != "$view_expected" ]]; then
    echo "unexpected tensor reshape/transpose autograd output:" >&2
    printf '%s\n' "$view_output" >&2
    exit 1
fi

cat > "$TMP/tensor-scatter-autograd.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> values = tensor.zeros<real32>([3]).track()
tensor<real32> seeded = values.untrack()
seeded[0] = real32(1)
seeded[1] = real32(2)
seeded[2] = real32(3)
values = seeded.track()
tensor<real32> scattered = values.scatter([0, 0, 2], [4])
tensor<real32> scattered_values = scattered.untrack()
print(scattered_values[0].item() == real32(3))
print(NL)
print(scattered_values[1].item() == real32(0))
print(NL)
print(scattered_values[2].item() == real32(3))
print(NL)
print(scattered_values[3].item() == real32(0))
print(NL)
reductions.mean((scattered * scattered)).backward(&values, track = true)
print(values.grad.is_tracked())
print(NL)
reductions.mean(values.grad).backward(&values)
print(values.grad.untrack().shape()[0] == 3)
print(NL)

tensor<int64> integer_values = tensor.zeros<int64>([3])
integer_values[0] = 1
integer_values[1] = 2
integer_values[2] = 3
tensor<int64> integer_scattered = integer_values.scatter([0, 0, 2], [4])
print(integer_scattered[0].item() == 3)
print(NL)
print(integer_scattered[1].item() == 0)
print(NL)
print(integer_scattered[2].item() == 3)
print(NL)
print(integer_scattered[3].item() == 0)
print(NL)

tensor<int64> long_values = tensor.ones<int64>([72])
int[] long_scatter_indices = array(72, fill = 0)
for index in range(72)
    long_scatter_indices[index] = index
tensor<int64> long_scattered = long_values.scatter(long_scatter_indices, [72])
print(long_scattered.shape()[0] == 72 and long_scattered[71].item() == 1)
print(NL)
QUI
scatter_output="$("$QUIDRA" "$TMP/tensor-scatter-autograd.qui")"
scatter_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$scatter_output" != "$scatter_expected" ]]; then
    echo "unexpected generic tensor scatter/autograd output:" >&2
    printf '%s\n' "$scatter_output" >&2
    exit 1
fi

cat > "$TMP/tensor-gather-autograd.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> source = tensor.ones<real32>([3]).track()
print(source.is_tracked())
print(NL)
tensor<real32> gathered = source.gather([2, 0, 2], [3])
print(gathered.is_tracked())
print(NL)
tensor<real32> loss = reductions.mean((gathered * gathered))
loss.backward(&source, track = true)
tensor<real32> first = source.grad
print(first.is_tracked())
print(NL)
tensor<real32> first_values = first.untrack()
print(first_values[0].item() > real32(0.66) and first_values[0].item() < real32(0.67))
print(NL)
print(first_values[1].item() == real32(0))
print(NL)
print(first_values[2].item() > real32(1.33) and first_values[2].item() < real32(1.34))
print(NL)
tensor<real32> second_loss = reductions.mean(first)
second_loss.backward(&source)
tensor<real32> accumulated = source.grad.untrack()
print(accumulated[0].item() > real32(0.88) and accumulated[0].item() < real32(0.90))
print(NL)
print(accumulated[1].item() == real32(0))
print(NL)
print(accumulated[2].item() > real32(1.77) and accumulated[2].item() < real32(1.79))
print(NL)

tensor<int64> long_source = tensor.ones<int64>([72])
int[] long_gather_indices = array(72, fill = 0)
for index in range(72)
    long_gather_indices[index] = index
tensor<int64> long_gathered = long_source.gather(long_gather_indices, [72])
print(long_gathered.shape()[0] == 72 and long_gathered[71].item() == 1)
print(NL)
QUI
gather_output="$("$QUIDRA" "$TMP/tensor-gather-autograd.qui")"
gather_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$gather_output" != "$gather_expected" ]]; then
    echo "unexpected generic tensor gather/autograd output:" >&2
    printf '%s\n' "$gather_output" >&2
    exit 1
fi

# With QUIDRA_SAVED_TENSORS=cow, CPU autograd nodes share the storage of the
# tensors they snapshot and the host engine shares gradient storage with
# slots and custom callbacks. Every later write must detach, so backward
# still sees the forward-time values, and gradients handed out must never
# change afterwards. QUIDRA_SAVED_TENSORS=copy copies instead; both modes
# must print the same.
cat > "$TMP/tensor-autograd-shared-snapshots.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> a = tensor.ones<real32>([3]) * real32(2)
tensor<real32> b = tensor.ones<real32>([3]) * real32(3)
tensor<real32> x = a.track()
tensor<real32> y = b.track()
tensor<real32> product = x * y
tensor<real32> loss = reductions.mean(product * x)
a[0] = real32(100)
b[1] = real32(100)
tensor<real32> plain = product.untrack()
plain[2] = real32(-1)
loss.backward(&a, &b)
tensor<real32> a_grad = a.grad
tensor<real32> b_grad = b.grad
print(a_grad[0].item() == real32(4) and a_grad[1].item() == real32(4) and a_grad[2].item() == real32(4))
print(NL)
print(b_grad[0].item() > real32(1.3333) and b_grad[0].item() < real32(1.3334))
print(NL)
a_grad[1] = real32(7)
print(a.grad[1].item() == real32(4))
print(NL)
loss.backward(&a)
print(a.grad[0].item() == real32(8) and a_grad[0].item() == real32(4))
print(NL)
print(product.untrack()[2].item() == real32(6) and plain[2].item() == real32(-1))
print(NL)
// Add hands both parents the same gradient buffer (here one the Mul
// backward allocated). A parent that later accumulates another contribution
// copies it instead of changing its sibling's, and keeps its full size for
// its own (scalar) backward.
tensor<real32> fives = tensor.ones<real32>([3]) * real32(5)
tensor<real32> c = tensor.ones<real32>([3]) * real32(2)
tensor<real32> d = tensor.ones<real32>([3]) * real32(3)
tensor<real32> u = c.track() * real32(2)
tensor<real32> v = d.track() * real32(3)
reductions.mean((u + v) * fives + u * v).backward(&c, &d)
// d/dc = 2 (5 + v) / 3 = 28/3, d/dd = 3 (5 + u) / 3 = 9
print(c.grad[0].item() > real32(9.3333) and c.grad[0].item() < real32(9.3334) and c.grad[2].item() > real32(9.3333))
print(NL)
print(d.grad[1].item() > real32(8.9999) and d.grad[1].item() < real32(9.0001) and d.grad[2].item() > real32(8.9999))
print(NL)
tensor<real32> e = tensor.ones<real32>([3]) * real32(2)
tensor<real32> f = tensor.ones<real32>([3]) * real32(3)
tensor<real32> p = e.track() * real32(2)
tensor<real32> q = f.track() * real32(3)
reductions.mean(p * q + (p + q) * fives).backward(&e, &f)
print(e.grad[0].item() > real32(9.3333) and e.grad[0].item() < real32(9.3334) and f.grad[1].item() > real32(8.9999) and f.grad[1].item() < real32(9.0001))
print(NL)
QUI
for saved_mode in copy cow; do
    shared_output="$(QUIDRA_SAVED_TENSORS="$saved_mode" "$QUIDRA" "$TMP/tensor-autograd-shared-snapshots.qui")"
    if [[ "$shared_output" != "$(for _ in {1..8}; do echo true; done)" ]]; then
        echo "unexpected shared autograd snapshot output ($saved_mode):" >&2
        printf '%s\n' "$shared_output" >&2
        exit 1
    fi
done

echo "tensor autograd foundation integration: ok"