#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/backward-target-required.qui" <<'QUI'
tensor<float32> x = tensor.ones<float32>([]).track()
tensor<float32> loss = x * x
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
const tensor<float32> x = tensor.ones<float32>([]).track()
tensor<float32> loss = x * x
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
tensor<float32> x = tensor.ones<float32>([]).track()
tensor<float32> loss = x * x
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
const tensor<float32> x = tensor.ones<float32>([]).track()
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
class Leaf
    autograd.Target target

    construct()
        target = autograd.target()

class Model
    Leaf[2] leaves

    construct()
        leaves = [Leaf(), Leaf()]

Model model = Model()
tensor<float32> left = tensor.ones<float32>([]).track(
    &model.leaves[0].target
)
tensor<float32> right = tensor.ones<float32>([]).track(
    &model.leaves[1].target
)
tensor<float32> loss = left * right
loss.backward(&model)
print(model.leaves[0].target.has_grad())
print(model.leaves[1].target.has_grad())
QUI
fixed_array_backward_output="$("$QUIDRA" "$TMP/fixed-array-backward-targets.qui")"
fixed_array_backward_expected="$(printf 'true\ntrue')"
if [[ "$fixed_array_backward_output" != "$fixed_array_backward_expected" ]]; then
    echo "unexpected fixed-array backward target output:" >&2
    printf '%s\n' "$fixed_array_backward_output" >&2
    exit 1
fi

cat > "$TMP/runtime-array-backward-targets.qui" <<'QUI'
class Leaf
    autograd.Target target

    construct()
        target = autograd.target()

class Model
    Leaf[] leaves

    construct()
        leaves = []
        leaves = leaves.append(Leaf())
        leaves = leaves.append(Leaf())

Model model = Model()
tensor<float32> left = tensor.ones<float32>([]).track(
    &model.leaves[0].target
)
tensor<float32> right = tensor.ones<float32>([]).track(
    &model.leaves[1].target
)
tensor<float32> loss = left * right
loss.backward(&model)
print(model.leaves[0].target.has_grad())
print(model.leaves[1].target.has_grad())
QUI
runtime_array_backward_output="$("$QUIDRA" "$TMP/runtime-array-backward-targets.qui")"
runtime_array_backward_expected="$(printf 'true\ntrue')"
if [[ "$runtime_array_backward_output" != "$runtime_array_backward_expected" ]]; then
    echo "unexpected runtime-array backward target output:" >&2
    printf '%s\n' "$runtime_array_backward_output" >&2
    exit 1
fi

cat > "$TMP/matmul-autograd.qui" <<'QUI'
tensor<float32> left = tensor.ones<float32>([2, 3]).track()
tensor<float32> right = tensor.ones<float32>([3, 2]).track()
tensor<float32> product = left.matmul(right)
product.mean().backward(&left, &right)
print(product.shape()[0] == 2 and product.shape()[1] == 2)
print(left.grad[0, 0].item() == float32(0.5))
print(right.grad[0, 0].item() == float32(0.5))

tensor<float32> vector_left = tensor.ones<float32>([2]).track()
tensor<float32> matrix_right = tensor.ones<float32>([2, 1]).track()
vector_left.matmul(matrix_right).mean().backward(&vector_left, &matrix_right)
print(vector_left.grad[0].item() == float32(1))
print(matrix_right.grad[0, 0].item() == float32(1))

tensor<float32> matrix_left = tensor.ones<float32>([1, 2]).track()
tensor<float32> vector_right = tensor.ones<float32>([2]).track()
matrix_left.matmul(vector_right).mean().backward(&matrix_left, &vector_right)
print(matrix_left.grad[0, 0].item() == float32(1))
print(vector_right.grad[0].item() == float32(1))

tensor<float32> batched_left = tensor.ones<float32>([2, 2, 3]).track()
tensor<float32> batched_right = tensor.ones<float32>([3, 4]).track()
tensor<float32> batched = batched_left.matmul(batched_right)
batched.mean().backward(&batched_left, &batched_right)
print(batched.shape()[0] == 2 and batched.shape()[1] == 2 and batched.shape()[2] == 4)
print(batched_left.grad[1, 1, 2].item() == float32(0.25))
print(batched_right.grad[2, 3].item() == float32(0.25))

tensor<float32> dot_left = tensor.ones<float32>([2]).track()
tensor<float32> dot_right = tensor.ones<float32>([2]).track()
tensor<float32> dot_product = dot_left.matmul(dot_right)
dot_product.backward(&dot_left, &dot_right)
print(len(dot_product.shape()) == 0)
print(dot_left.grad[0].item() == float32(1))
print(dot_right.grad[0].item() == float32(1))

tensor<float32> high_left = tensor.ones<float32>([1, 1]).track()
tensor<float32> high_right = tensor.ones<float32>([1, 1]).track()
tensor<float32> high = high_left.matmul(high_right)
(high * high).mean().backward(&high_left, &high_right, track = true)
tensor<float32> first = high_left.grad
print(first.is_tracked())
first.mean().backward(&high_left)
print(high_left.grad.untrack()[0, 0].item() == float32(4))
QUI

matmul_autograd_output="$("$QUIDRA" "$TMP/matmul-autograd.qui")"
matmul_autograd_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$matmul_autograd_output" != "$matmul_autograd_expected" ]]; then
    echo "unexpected tensor matmul autograd output:" >&2
    printf '%s\n' "$matmul_autograd_output" >&2
    exit 1
fi

cat > "$TMP/broadcast-autograd.qui" <<'QUI'
tensor<float32> left_values = tensor.zeros<float32>([2, 1])
left_values[0, 0] = float32(2)
left_values[1, 0] = float32(3)
tensor<float32> left = left_values.track()
tensor<float32> right = tensor.ones<float32>([2, 3]).track()
tensor<float32> output = left * right
print(output.shape()[0] == 2 and output.shape()[1] == 3)
output.mean().backward(&left, &right, track = true)
print(left.grad.is_tracked())
print(right.grad.is_tracked())
tensor<float32> left_grad = left.grad.untrack()
tensor<float32> right_grad = right.grad.untrack()
print(left_grad[0, 0].item() == float32(0.5))
print(left_grad[1, 0].item() == float32(0.5))
print(right_grad[0, 0].item() > float32(0.3333) and right_grad[0, 0].item() < float32(0.3334))
print(right_grad[1, 2].item() == float32(0.5))
left.grad.mean().backward(&right)
tensor<float32> accumulated = right.grad.untrack()
print(accumulated[0, 0].item() > float32(0.4166) and accumulated[0, 0].item() < float32(0.4168))
print(accumulated[1, 2].item() > float32(0.5832) and accumulated[1, 2].item() < float32(0.5834))
QUI

broadcast_autograd_output="$("$QUIDRA" "$TMP/broadcast-autograd.qui")"
broadcast_autograd_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$broadcast_autograd_output" != "$broadcast_autograd_expected" ]]; then
    echo "unexpected tensor broadcast autograd output:" >&2
    printf '%s\n' "$broadcast_autograd_output" >&2
    exit 1
fi

cat > "$TMP/tensor-convolve.qui" <<'QUI'
tensor<float32> source_signal = tensor.zeros<float32>([4])
source_signal[0] = float32(1)
source_signal[1] = float32(2)
source_signal[2] = float32(3)
source_signal[3] = float32(4)
tensor<float32> kernel = tensor.zeros<float32>([2])
kernel[0] = float32(10)
kernel[1] = float32(1)
tensor<float32> convolved = source_signal.convolve(kernel)
print(convolved.shape()[0] == 3)
print(convolved[0].item() == float32(21))
print(convolved[1].item() == float32(32))
print(convolved[2].item() == float32(43))

tensor<float32> planes = tensor.ones<float32>([2, 3, 3])
tensor<float32> square = tensor.ones<float32>([2, 2])
tensor<float32> plane_output = planes.convolve(square)
print(plane_output.shape()[0] == 2)
print(plane_output.shape()[1] == 2)
print(plane_output.shape()[2] == 2)
print(plane_output[1, 1, 1].item() == float32(4))

tensor<float32> dilated_source = tensor.zeros<float32>([5])
dilated_source[0] = float32(1)
dilated_source[1] = float32(2)
dilated_source[2] = float32(3)
dilated_source[3] = float32(4)
dilated_source[4] = float32(5)
tensor<float32> dilated_kernel = tensor.ones<float32>([2])
tensor<float32> dilated = dilated_source.convolve(
    dilated_kernel, dilation = 2
)
print(dilated.shape()[0] == 3)
print(dilated[0].item() == float32(4))
print(dilated[1].item() == float32(6))
print(dilated[2].item() == float32(8))

tensor<float32> padded = source_signal.convolve(kernel, padding = 1)
print(padded.shape()[0] == 5)
print(padded[0].item() == float32(10))
print(padded[4].item() == float32(4))

tensor<int> integer_signal = tensor.zeros<int>([4])
integer_signal[0] = 1
integer_signal[1] = 2
integer_signal[2] = 3
integer_signal[3] = 4
tensor<int> integer_kernel = tensor.zeros<int>([2])
integer_kernel[0] = 10
integer_kernel[1] = 1
tensor<int> integer_convolved = integer_signal.convolve(integer_kernel)
print(integer_convolved.shape()[0] == 3)
print(integer_convolved[0].item() == 21)
print(integer_convolved[1].item() == 32)
print(integer_convolved[2].item() == 43)
tensor<int> integer_padded = integer_signal.convolve(integer_kernel, padding = 1)
print(integer_padded[0].item() == 10)
print(integer_padded[4].item() == 4)

tensor<float32> high_source = tensor.ones<float32>([3]).track()
tensor<float32> high_kernel = tensor.ones<float32>([2]).track()
tensor<float32> high_output = high_source.convolve(high_kernel)
(high_output * high_output).mean().backward(&high_source, &high_kernel, track = true)
print(high_source.grad.is_tracked())
print(high_kernel.grad.is_tracked())
high_source.grad.mean().backward(&high_source)
print(high_source.grad.untrack().shape()[0] == 3)
QUI

tensor_convolve_output="$("$QUIDRA" "$TMP/tensor-convolve.qui")"
tensor_convolve_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tensor_convolve_output" != "$tensor_convolve_expected" ]]; then
    echo "unexpected generic tensor convolve output:" >&2
    printf '%s\n' "$tensor_convolve_output" >&2
    exit 1
fi

cat > "$TMP/linear-primitives.qui" <<'QUI'
tensor<float32> weight = tensor.ones<float32>([1, 2])
tensor<float32> bias = tensor.zeros<float32>([1])
tensor<float32> samples = tensor.ones<float32>([1, 2])
tensor<float32> output = samples.matmul(weight.transpose(0, 1))
output = output + bias.gather([0], [1, 1])
print(output[0, 0].item())
QUI

linear_primitives_output="$("$QUIDRA" "$TMP/linear-primitives.qui")"
if [[ "$linear_primitives_output" != "2.0" ]]; then
    echo "unexpected generic linear primitive output: $linear_primitives_output" >&2
    exit 1
fi

cat > "$TMP/autograd-contracts.qui" <<'QUI'
tensor<float32> weight = tensor.ones<float32>([1, 2])
tensor<float32> bias = tensor.zeros<float32>([1])
tensor<float32> samples = tensor.ones<float32>([1, 2])
tensor<float32> tracked = samples.track()
tensor<float32> tracked_weight = weight.track()
tensor<float32> tracked_bias = bias.track()
tensor<float32> prediction = tracked.matmul(tracked_weight.transpose(0, 1))
prediction = prediction + tracked_bias.gather([0], [1, 1])
prediction.mean().backward(&tracked, &weight, &bias)

print(tracked.grad[0, 0].item())
print(tracked.grad[0, 1].item())
print(weight.grad[0, 0].item())
print(weight.grad[0, 1].item())
print(bias.grad[0].item())

prediction.mean().backward(&tracked, &weight, &bias)
print(weight.grad[0, 0].item())

tensor<float32> tracked_value = tensor.ones<float32>([]).track()
tensor<float32> constant_value = tensor.ones<float32>([]) * float32(4)
tensor<float32> mixed = tracked_value * constant_value
mixed.backward(&tracked_value)
print(tracked_value.grad.item())
QUI

autograd_contract_output="$("$QUIDRA" "$TMP/autograd-contracts.qui")"
autograd_contract_expected="$(printf '1.0\n1.0\n1.0\n1.0\n1.0\n2.0\n4.0')"
if [[ "$autograd_contract_output" != "$autograd_contract_expected" ]]; then
    echo "unexpected tensor autograd contract output:" >&2
    printf '%s\n' "$autograd_contract_output" >&2
    exit 1
fi

cat > "$TMP/untracked-forward.qui" <<'QUI'
tensor<float32> weight = tensor.ones<float32>([1, 2])
tensor<float32> bias = tensor.zeros<float32>([1])
tensor<float32> samples = tensor.ones<float32>([1, 2])
tensor<float32> prediction = samples.matmul(weight.transpose(0, 1))
prediction = prediction + bias.gather([0], [1, 1])
prediction.mean().backward(&samples)
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
tensor<float32> source = tensor.ones<float32>([1])
print(source.grad[0].item())
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
tensor<float32> root = tensor.ones<float32>([]).track()
tensor<float32> before_cut = root * float32(2)
tensor<float32> after_cut = before_cut.retrack()
tensor<float32> loss = after_cut * float32(3)
loss.backward(&after_cut, &root)
print(after_cut.grad.item())
print(root.grad.item())
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
tensor<float32> root = tensor.ones<float32>([]).track()
tensor<float32> loss = root * float32(2)
print(not root.has_grad())
loss.backward(&root)
print(root.has_grad())
print(root.grad.untrack().item() == float32(2))
root.clear_grad()
print(not root.has_grad())
loss.backward(&root)
print(root.has_grad())
print(root.grad.untrack().item() == float32(2))
QUI
tensor_grad_state_output="$("$QUIDRA" "$TMP/tensor-grad-state.qui")"
tensor_grad_state_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tensor_grad_state_output" != "$tensor_grad_state_expected" ]]; then
    echo "unexpected tensor gradient state output:" >&2
    printf '%s\n' "$tensor_grad_state_output" >&2
    exit 1
fi

cat > "$TMP/explicit-gradient-selection-and-accumulation.qui" <<'QUI'
tensor<float32> x = tensor.ones<float32>([]).track()
tensor<float32> y_value = tensor.ones<float32>([]) * float32(3)
tensor<float32> y = y_value.track()
tensor<float32> loss = x * y

loss.backward(&x)
print(x.grad.item() == float32(3))
print(not y.has_grad())

loss.backward(&x)
print(x.grad.item() == float32(6))
print(not y.has_grad())

x.clear_grad()
loss.backward(&x, &y)
print(x.grad.item() == float32(3))
print(y.grad.item() == float32(1))
QUI
selection_accumulation_output="$("$QUIDRA" run "$TMP/explicit-gradient-selection-and-accumulation.qui")"
selection_accumulation_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$selection_accumulation_output" != "$selection_accumulation_expected" ]]; then
    echo "unexpected explicit gradient selection/accumulation output:" >&2
    printf '%s\n' "$selection_accumulation_output" >&2
    exit 1
fi

cat > "$TMP/untrack-cut.qui" <<'QUI'
tensor<float32> root = tensor.ones<float32>([]).track()
tensor<float32> detached = (root * float32(2)).untrack()
tensor<float32> loss = detached * float32(3)
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
tensor<float32> tracked = tensor.ones<float32>([]).track()
tensor<float32> constant = tensor.ones<float32>([]) * float32(4)
tensor<float32> loss = tracked * constant
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
tensor<float32> tracked = tensor.ones<float32>([2]).track()
tracked[0] = float32(2)
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
            body='tensor<float32> ignored = tracked.contiguous()'
            ;;
        indexing)
            body='tensor<float32> ignored = tracked[0:1, 0:2]'
            ;;
    esac
    cat > "$TMP/tracked-transform.qui" <<QUI
tensor<float32> tracked = tensor.ones<float32>([2, 2]).track()
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

cat > "$TMP/math.qui" <<'QUI'
tensor<float32> values = tensor.zeros<float32>([1, 3])
values[0, 0] = -1.0
values[0, 1] = 2.0
values[0, 2] = 3.0
tensor<float32> tracked = values.track()
tensor<float32> positive = tracked.abs()
tensor<float32> restored = positive.exp().log()
tensor<float32> sums = restored.sum_last()
tensor<float32> maxima = restored.max_last()
tensor<float32> average = restored.mean()
average.backward(&tracked)
print(math.abs(float(restored.untrack()[0, 0].item()) - 1.0) < 0.000001)
print(sums.untrack()[0, 1].item())
print(maxima.untrack()[0, 2].item())
print(average.untrack().item())
QUI

math_output="$("$QUIDRA" "$TMP/math.qui")"
math_expected="$(printf 'true\n6.0\n3.0\n2.0')"
if [[ "$math_output" != "$math_expected" ]]; then
    echo "unexpected tensor autograd math output: $math_output" >&2
    exit 1
fi

cat > "$TMP/tensor-extrema.qui" <<'QUI'
tensor<int> values = tensor.zeros<int>([2, 3])
values[0, 0] = 5
values[0, 1] = -2
values[0, 2] = 4
values[1, 0] = 9
values[1, 1] = 3
values[1, 2] = 7
tensor<int> maxima = values.max_last()
tensor<int> minima = values.min_last()
print(maxima[0, 0].item() == 5)
print(maxima[1, 2].item() == 9)
print(minima[0, 1].item() == -2)
print(minima[1, 0].item() == 3)

tensor<float32> floats = tensor.zeros<float32>([1, 3])
floats[0, 0] = float32(2.5)
floats[0, 1] = float32(-1.5)
floats[0, 2] = float32(4.0)
print(floats.max_last()[0, 1].item() == float32(4.0))
print(floats.min_last()[0, 2].item() == float32(-1.5))
QUI
extrema_output="$("$QUIDRA" "$TMP/tensor-extrema.qui")"
extrema_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$extrema_output" != "$extrema_expected" ]]; then
    echo "unexpected tensor extrema output: $extrema_output" >&2
    exit 1
fi

cat > "$TMP/tensor-grad-copy-isolation.qui" <<'QUI'
tensor<float32> source = tensor.ones<float32>([2])
tensor<float32> alias = source
tensor<float32> tracked = source.track()
(tracked * tracked).mean().backward(&source)
print(source.has_grad())
print(not alias.has_grad())
tensor<float32> gradient = source.grad
print(gradient[0].item() == float32(1))
print(gradient[1].item() == float32(1))
source.clear_grad()
print(not source.has_grad())
print(not alias.has_grad())
QUI
tensor_grad_slot_output="$("$QUIDRA" run "$TMP/tensor-grad-copy-isolation.qui")"
tensor_grad_slot_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tensor_grad_slot_output" != "$tensor_grad_slot_expected" ]]; then
    echo "unexpected tensor grad-slot output:" >&2
    printf '%s\n' "$tensor_grad_slot_output" >&2
    exit 1
fi

cat > "$TMP/tracked-copy-gradient-identity.qui" <<'QUI'
tensor<float32> source = tensor.ones<float32>([1]).track()
tensor<float32> copy = source
tensor<float32> source_loss = source * float32(2)
source_loss.backward(&source)
print(source.has_grad())
print(not copy.has_grad())

tensor<float32> copy_loss = copy * float32(3)
copy_loss.backward(&copy)
print(copy.has_grad())
print(copy.grad[0].item() == float32(3))
print(source.grad[0].item() == float32(2))

source.clear_grad()
copy.clear_grad()
source_loss.backward(&source, &copy)
print(source.grad[0].item() == float32(2))
print(copy.grad[0].item() == float32(2))

source.clear_grad()
source_loss.backward(&source, &source)
print(source.grad[0].item() == float32(2))

tensor<float32> explicit_track = source.track()
tensor<float32> explicit_loss = explicit_track * float32(4)
explicit_loss.backward(&explicit_track)
print(explicit_track.has_grad())
print(explicit_track.grad[0].item() == float32(4))
print(source.grad[0].item() == float32(2))
QUI
tracked_copy_output="$("$QUIDRA" run "$TMP/tracked-copy-gradient-identity.qui")"
tracked_copy_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$tracked_copy_output" != "$tracked_copy_expected" ]]; then
    echo "unexpected tracked tensor copy gradient identity output:" >&2
    printf '%s\n' "$tracked_copy_output" >&2
    exit 1
fi

cat > "$TMP/tracked-min-last.qui" <<'QUI'
tensor<float32> values = tensor.zeros<float32>([2])
values[0] = float32(2)
values[1] = float32(1)
tensor<float32> tracked = values.track()
tensor<float32> reduced = tracked.min_last()
reduced.mean().backward(&tracked)
tensor<float32> gradient = tracked.grad.untrack()
print(reduced.untrack()[0].item() == float32(1))
print(gradient[0].item() == float32(0))
print(gradient[1].item() == float32(1))
QUI
tracked_min_output="$("$QUIDRA" "$TMP/tracked-min-last.qui")"
if [[ "$tracked_min_output" != "$(printf 'true\ntrue\ntrue')" ]]; then
    echo "unexpected tracked min_last first-order output:" >&2
    printf '%s\n' "$tracked_min_output" >&2
    exit 1
fi

cat > "$TMP/tracked-extrema-higher-order.qui" <<'QUI'
tensor<float32> min_values = tensor.zeros<float32>([2])
min_values[0] = float32(2)
min_values[1] = float32(1)
tensor<float32> min_tracked = min_values.track()
tensor<float32> min_reduced = min_tracked.min_last()
(min_reduced * min_reduced).mean().backward(&min_tracked, track = true)
tensor<float32> min_first = min_tracked.grad
print(min_first.is_tracked())
print(min_first.untrack()[0].item() == float32(0))
print(min_first.untrack()[1].item() == float32(2))
min_first.mean().backward(&min_tracked)
tensor<float32> min_accumulated = min_tracked.grad.untrack()
print(min_accumulated[0].item() == float32(0))
print(min_accumulated[1].item() == float32(3))

tensor<float32> max_values = tensor.zeros<float32>([2])
max_values[0] = float32(2)
max_values[1] = float32(1)
tensor<float32> max_tracked = max_values.track()
tensor<float32> max_reduced = max_tracked.max_last()
(max_reduced * max_reduced).mean().backward(&max_tracked, track = true)
tensor<float32> max_first = max_tracked.grad
print(max_first.is_tracked())
print(max_first.untrack()[0].item() == float32(4))
print(max_first.untrack()[1].item() == float32(0))
max_first.mean().backward(&max_tracked)
tensor<float32> max_accumulated = max_tracked.grad.untrack()
print(max_accumulated[0].item() == float32(5))
print(max_accumulated[1].item() == float32(0))
QUI
extrema_higher_output="$("$QUIDRA" "$TMP/tracked-extrema-higher-order.qui")"
extrema_higher_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$extrema_higher_output" != "$extrema_higher_expected" ]]; then
    echo "unexpected tracked extrema higher-order output:" >&2
    printf '%s\n' "$extrema_higher_output" >&2
    exit 1
fi

cat > "$TMP/tracked-dtype-cast.qui" <<'QUI'
tensor<float32> source = tensor.ones<float32>([2])
tensor<float32> tracked = source.track()
tensor<float> invalid = float(tracked)
print(invalid.shape()[0])
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
tensor<float32> source = tensor.ones<float32>([2])
tensor<float32> tracked = source.track()
tensor<float> converted = float(tracked.untrack())
print(converted.shape()[0])
print(converted[0].item())
QUI
untracked_cast_output="$("$QUIDRA" "$TMP/untracked-dtype-cast.qui")"
if [[ "$untracked_cast_output" != "$(printf '2\n1.0')" ]]; then
    echo "unexpected explicit-untrack dtype cast output: $untracked_cast_output" >&2
    exit 1
fi

cat > "$TMP/value-copy-autograd.qui" <<'QUI'
tensor<float32> tensor_original = tensor.ones<float32>([2])
tensor<float32> tensor_copy = tensor_original
print(&tensor_original != &tensor_copy)
tensor_copy[0] = 9.0
print(tensor_original[0].item())
print(tensor_copy[0].item())
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

tensor<float32> values = tensor.ones<float32>([1, 3])
tensor<float32> tracked = values.track()
print(identity(tracked.abs()).untrack().shape()[1])
print(identity(tracked.exp()).untrack().shape()[1])
print(identity(tracked.abs().log()).untrack().shape()[1])
print(identity(tracked.sum_last()).untrack().shape()[1])
print(identity(tracked.max_last()).untrack().shape()[1])
QUI

inference_output="$("$QUIDRA" "$TMP/primitive-inference.qui")"
if [[ "$inference_output" != "$(printf '3\n3\n3\n3\n3')" ]]; then
    echo "unexpected tensor primitive inference output: $inference_output" >&2
    exit 1
fi

cat > "$TMP/tracked-mutation.qui" <<'QUI'
tensor<float32> value = tensor.ones<float32>([2]).track()
value[0] = float32(3)
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
tensor<float32> value = tensor.ones<float32>([2]).track()
tensor<float32> alias = value
alias[0] = float32(3)
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
            body='tensor<float32> changed = value.contiguous()'
            expected='contiguous() on a tracked tensor requires explicit untrack() first'
            ;;
        index)
            body='tensor<float32> changed = value[0]'
            expected='indexing on a tracked tensor requires explicit untrack() first'
            ;;
    esac
    cat > "$TMP/tracked-transform.qui" <<QUI
tensor<float32> value = tensor.ones<float32>([1, 2]).track()
$body
print(changed.shape()[0])
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
tensor<float32> value = tensor.ones<float32>([2]).track().untrack()
value[0] = float32(3)
print(value[0].item())
QUI
if [[ "$("$QUIDRA" "$TMP/untracked-mutation.qui")" != "3.0" ]]; then
    echo "explicitly untracked tensor mutation failed" >&2
    exit 1
fi

cat > "$TMP/higher-order.qui" <<'QUI'
tensor<float32> x = tensor.ones<float32>([]).track()
tensor<float32> loss = x * x * x
loss.backward(&x, track = true)
print(x.grad.item())
tensor<float32> first = x.grad
first.backward(&x, track = true)
print(x.grad.item())
tensor<float32> second = x.grad
second.backward(&x)
print(x.grad.item())
QUI

higher_order_output="$("$QUIDRA" "$TMP/higher-order.qui")"
higher_order_expected="$(printf '3.0\n9.0\n21.0')"
if [[ "$higher_order_output" != "$higher_order_expected" ]]; then
    echo "unexpected higher-order tensor autograd output:" >&2
    printf '%s\n' "$higher_order_output" >&2
    exit 1
fi

cat > "$TMP/tensor-view-autograd.qui" <<'QUI'
tensor<float32> source_values = tensor.zeros<float32>([2, 3])
source_values[0, 0] = float32(1)
source_values[0, 1] = float32(2)
source_values[0, 2] = float32(3)
source_values[1, 0] = float32(4)
source_values[1, 1] = float32(5)
source_values[1, 2] = float32(6)
tensor<float32> source = source_values.track()
tensor<float32> transposed = source.transpose(0, 1)
print(transposed.is_tracked())
print(transposed.shape()[0] == 3 and transposed.shape()[1] == 2)
print(transposed.untrack()[0, 1].item() == float32(4))
tensor<float32> loss = (transposed * transposed).mean()
loss.backward(&source, track = true)
tensor<float32> first = source.grad
print(first.is_tracked())
tensor<float32> first_values = first.untrack()
print(first_values[0, 0].item() > float32(0.33) and first_values[0, 0].item() < float32(0.34))
print(first_values[1, 2].item() == float32(2))
first.mean().backward(&source)
tensor<float32> accumulated = source.grad.untrack()
print(accumulated[0, 0].item() > float32(0.38) and accumulated[0, 0].item() < float32(0.40))
print(accumulated[1, 2].item() > float32(2.05) and accumulated[1, 2].item() < float32(2.06))

tensor<float32> reshape_source = tensor.ones<float32>([2, 3]).track()
tensor<float32> reshaped = reshape_source.reshape([3, 2])
print(reshaped.is_tracked())
(reshaped * reshaped).mean().backward(&reshape_source, track = true)
tensor<float32> reshape_first = reshape_source.grad
print(reshape_first.is_tracked())
tensor<float32> reshape_values = reshape_first.untrack()
print(reshape_values[0, 0].item() > float32(0.33) and reshape_values[0, 0].item() < float32(0.34))
reshape_first.mean().backward(&reshape_source)
tensor<float32> reshape_accumulated = reshape_source.grad.untrack()
print(reshape_accumulated[0, 0].item() > float32(0.38) and reshape_accumulated[0, 0].item() < float32(0.40))
QUI
view_output="$("$QUIDRA" "$TMP/tensor-view-autograd.qui")"
view_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$view_output" != "$view_expected" ]]; then
    echo "unexpected tensor reshape/transpose autograd output:" >&2
    printf '%s\n' "$view_output" >&2
    exit 1
fi

cat > "$TMP/tensor-scatter-autograd.qui" <<'QUI'
tensor<float32> values = tensor.zeros<float32>([3]).track()
tensor<float32> seeded = values.untrack()
seeded[0] = float32(1)
seeded[1] = float32(2)
seeded[2] = float32(3)
values = seeded.track()
tensor<float32> scattered = values.scatter([0, 0, 2], [4])
tensor<float32> scattered_values = scattered.untrack()
print(scattered_values[0].item() == float32(3))
print(scattered_values[1].item() == float32(0))
print(scattered_values[2].item() == float32(3))
print(scattered_values[3].item() == float32(0))
(scattered * scattered).mean().backward(&values, track = true)
print(values.grad.is_tracked())
values.grad.mean().backward(&values)
print(values.grad.untrack().shape()[0] == 3)

tensor<int> integer_values = tensor.zeros<int>([3])
integer_values[0] = 1
integer_values[1] = 2
integer_values[2] = 3
tensor<int> integer_scattered = integer_values.scatter([0, 0, 2], [4])
print(integer_scattered[0].item() == 3)
print(integer_scattered[1].item() == 0)
print(integer_scattered[2].item() == 3)
print(integer_scattered[3].item() == 0)

tensor<int> long_values = tensor.ones<int>([72])
int[] long_scatter_indices = array(72, fill = 0)
for index in range(72)
    long_scatter_indices[index] = index
tensor<int> long_scattered = long_values.scatter(long_scatter_indices, [72])
print(long_scattered.shape()[0] == 72 and long_scattered[71].item() == 1)
QUI
scatter_output="$("$QUIDRA" "$TMP/tensor-scatter-autograd.qui")"
scatter_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$scatter_output" != "$scatter_expected" ]]; then
    echo "unexpected generic tensor scatter/autograd output:" >&2
    printf '%s\n' "$scatter_output" >&2
    exit 1
fi

cat > "$TMP/tensor-gather-autograd.qui" <<'QUI'
tensor<float32> source = tensor.ones<float32>([3]).track()
print(source.is_tracked())
tensor<float32> gathered = source.gather([2, 0, 2], [3])
print(gathered.is_tracked())
tensor<float32> loss = (gathered * gathered).mean()
loss.backward(&source, track = true)
tensor<float32> first = source.grad
print(first.is_tracked())
tensor<float32> first_values = first.untrack()
print(first_values[0].item() > float32(0.66) and first_values[0].item() < float32(0.67))
print(first_values[1].item() == float32(0))
print(first_values[2].item() > float32(1.33) and first_values[2].item() < float32(1.34))
tensor<float32> second_loss = first.mean()
second_loss.backward(&source)
tensor<float32> accumulated = source.grad.untrack()
print(accumulated[0].item() > float32(0.88) and accumulated[0].item() < float32(0.90))
print(accumulated[1].item() == float32(0))
print(accumulated[2].item() > float32(1.77) and accumulated[2].item() < float32(1.79))

tensor<int> long_source = tensor.ones<int>([72])
int[] long_gather_indices = array(72, fill = 0)
for index in range(72)
    long_gather_indices[index] = index
tensor<int> long_gathered = long_source.gather(long_gather_indices, [72])
print(long_gathered.shape()[0] == 72 and long_gathered[71].item() == 1)
QUI
gather_output="$("$QUIDRA" "$TMP/tensor-gather-autograd.qui")"
gather_expected="$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue')"
if [[ "$gather_output" != "$gather_expected" ]]; then
    echo "unexpected generic tensor gather/autograd output:" >&2
    printf '%s\n' "$gather_output" >&2
    exit 1
fi

echo "tensor autograd foundation integration: ok"