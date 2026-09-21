#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/affine.qui" <<'QUI'
neural.Parameter<float32> weight = neural.Parameter<float32>(
    value = tensor.ones<float32>([1, 2])
)
neural.Parameter<float32> bias = neural.Parameter<float32>(
    value = tensor.zeros<float32>([1])
)
tensor<float32> samples = tensor.ones<float32>([1, 2])
neural<float32> output = neural.affine(neural.track(samples), weight, bias)
print(output.untrack()[0, 0].item())
QUI

affine_output="$("$QUIDRA" "$TMP/affine.qui")"
if [[ "$affine_output" != "2.0" ]]; then
    echo "unexpected affine output: $affine_output" >&2
    exit 1
fi

cat > "$TMP/math.qui" <<'QUI'
tensor<float32> values = tensor.zeros<float32>([1, 3])
values[0, 0] = -1.0
values[0, 1] = 2.0
values[0, 2] = 3.0
neural<float32> tracked = neural.track(values)
neural<float32> positive = neural.absolute(tracked)
neural<float32> restored = neural.logarithm(neural.exponential(positive))
neural<float32> sums = neural.sum_last(restored)
neural<float32> maxima = neural.max_last(restored)
neural<float32> average = neural.mean(restored)
neural.Gradients gradients = neural.grad(average)
print(math.abs(float(restored.untrack()[0, 0].item()) - 1.0) < 0.000001)
print(sums.untrack()[0, 1].item())
print(maxima.untrack()[0, 2].item())
print(average.untrack().item())
QUI

math_output="$("$QUIDRA" "$TMP/math.qui")"
math_expected="$(printf 'true\n6.0\n3.0\n2.0')"
if [[ "$math_output" != "$math_expected" ]]; then
    echo "unexpected neural math output: $math_output" >&2
    exit 1
fi

cat > "$TMP/update.qui" <<'QUI'
class Model
    neural.Parameter<float32> weight
    neural.Parameter<float32> bias

Model model = Model(
    weight = neural.Parameter<float32>(value = tensor.ones<float32>([1, 2])),
    bias = neural.Parameter<float32>(value = tensor.zeros<float32>([1]))
)
tensor<float32> samples = tensor.ones<float32>([1, 2])
neural<float32> prediction = neural.affine(
    neural.track(samples), model.weight, model.bias
)
neural<float32> loss = neural.mean(prediction * prediction)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
print(math.abs(float(model.weight.raw()[0, 0].item()) - 0.6) < 0.000001)
print(math.abs(float(model.bias.raw()[0].item()) + 0.4) < 0.000001)
QUI

update_output="$("$QUIDRA" "$TMP/update.qui")"
if [[ "$update_output" != "$(printf 'true\ntrue')" ]]; then
    echo "unexpected neural update output: $update_output" >&2
    exit 1
fi

cat > "$TMP/dtype-cast.qui" <<'QUI'
class Model
    neural.Parameter<float32> value

Model model = Model(
    value = neural.Parameter<float32>(value = tensor.ones<float32>([2]))
)
neural<float32><2> tracked = model.value.track()
neural<float><2> promoted = float(tracked)
tensor<float><2> restored = promoted.untrack()
print(restored.shape()[0])
print(restored[0].item())
neural<float> loss = neural.mean(promoted * promoted)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
print(math.abs(float(model.value.raw()[0].item()) - 0.9) < 0.000001)
QUI
dtype_cast_output="$("$QUIDRA" "$TMP/dtype-cast.qui")"
if [[ "$dtype_cast_output" != "$(printf '2\n1.0\ntrue')" ]]; then
    echo "unexpected neural dtype cast output: $dtype_cast_output" >&2
    exit 1
fi

cat > "$TMP/normalize.qui" <<'QUI'
neural.Parameter<float32> scale = neural.Parameter<float32>(
    value = tensor.ones<float32>([2])
)
neural.Parameter<float32> bias = neural.Parameter<float32>(
    value = tensor.zeros<float32>([2])
)
neural.State<tensor<float32>> running_mean = neural.State<tensor<float32>>(
    value = tensor.zeros<float32>([2])
)
neural.State<tensor<float32>> running_variance = neural.State<tensor<float32>>(
    value = tensor.ones<float32>([2])
)
tensor<float32> values = tensor.ones<float32>([2, 2])
neural<float32> normalized = neural.normalize(
    neural.track(values), scale, bias, running_mean, running_variance,
    momentum = 0.1, epsilon = 0.00001
)
tensor<float32> inference = neural.normalize_inference(
    values, scale, bias, running_mean, running_variance, epsilon = 0.00001
)
print(normalized.untrack().shape()[1])
print(inference.shape()[1])
print(running_mean.value[0].item() > float32(0))
QUI

normalize_output="$("$QUIDRA" "$TMP/normalize.qui")"
if [[ "$normalize_output" != "$(printf '2\n2\ntrue')" ]]; then
    echo "unexpected neural normalize output: $normalize_output" >&2
    exit 1
fi

cat > "$TMP/random-mask.qui" <<'QUI'
neural.State<uint64> rng = neural.State<uint64>(value = uint64(17))
tensor<float32> values = tensor.ones<float32>([8])
neural<float32> masked = neural.random_mask(neural.track(values), rng, rate = 0.5)
print(masked.untrack().shape()[0])
print(rng.value != uint64(17))
QUI

mask_output="$("$QUIDRA" "$TMP/random-mask.qui")"
if [[ "$mask_output" != "$(printf '8\ntrue')" ]]; then
    echo "unexpected neural random mask output: $mask_output" >&2
    exit 1
fi


cat > "$TMP/neural-scalar-grad.qui" <<'QUI'
class ScalarModel
    neural.Parameter<float32> value

ScalarModel model = ScalarModel(
    value = neural.Parameter<float32>(
        value = tensor.ones<float32>([1]) * float32(2)
    )
)
neural<float32> x = model.value.track()
neural<float32> transformed = (float32(5) - x * float32(3)) / float32(2)
neural<float32> reciprocal = float32(8) / x
neural<float32> loss = neural.mean(transformed + reciprocal)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
float32 updated = model.value.raw()[0].item()
print(updated > float32(2.3499) and updated < float32(2.3501))
QUI
if [[ "$("$QUIDRA" "$TMP/neural-scalar-grad.qui")" != "true" ]]; then
    echo "unexpected CPU scalar autograd result" >&2
    exit 1
fi

cat > "$TMP/value-copy-autograd.qui" <<'QUI'
tensor<float32> tensor_original = tensor.ones<float32>([2])
tensor<float32> tensor_copy = tensor_original
print(&tensor_original != &tensor_copy)
tensor_copy[0] = 9.0
print(tensor_original[0].item())
print(tensor_copy[0].item())

class AliasModel
    neural.Parameter<float32> weight

AliasModel unused_alias_model = AliasModel(
    weight = neural.Parameter<float32>(value = tensor.ones<float32>([1]))
)
neural<float32> unused_source = unused_alias_model.weight.track()
neural<float32> unused_alias = unused_source
neural<float32> unused_loss = neural.mean(unused_source + float32(1))
neural.Gradients unused_gradients = neural.grad(unused_loss)
neural.update(&unused_alias_model, unused_gradients, rate = 0.1)
print(math.abs(float(unused_alias_model.weight.raw()[0].item()) - 0.9) < 0.000001)
print(unused_alias.untrack()[0].item())

AliasModel shared_path_model = AliasModel(
    weight = neural.Parameter<float32>(value = tensor.ones<float32>([1]))
)
neural<float32> shared_source = shared_path_model.weight.track()
neural<float32> shared_alias = shared_source
neural<float32> shared_loss = neural.mean(shared_source + shared_alias)
neural.Gradients shared_gradients = neural.grad(shared_loss)
neural.update(&shared_path_model, shared_gradients, rate = 0.1)
print(math.abs(float(shared_path_model.weight.raw()[0].item()) - 0.8) < 0.000001)

class CopyModel
    neural.Parameter<float32> weight

CopyModel original_model = CopyModel(
    weight = neural.Parameter<float32>(value = tensor.ones<float32>([1]))
)
CopyModel copied_model = original_model
neural<float32> copied_value = copied_model.weight.track()
neural<float32> copied_loss = neural.mean(copied_value * copied_value)
neural.Gradients copied_gradients = neural.grad(copied_loss)
neural.update(&copied_model, copied_gradients, rate = 0.1)
print(original_model.weight.raw()[0].item())
print(math.abs(float(copied_model.weight.raw()[0].item()) - 0.8) < 0.000001)
QUI

value_copy_output="$("$QUIDRA" "$TMP/value-copy-autograd.qui")"
value_copy_expected="$(printf 'true\n1.0\n9.0\ntrue\n1.0\ntrue\n1.0\ntrue')"
if [[ "$value_copy_output" != "$value_copy_expected" ]]; then
    echo "unexpected value-copy/autograd output:" >&2
    printf '%s\n' "$value_copy_output" >&2
    exit 1
fi

cat > "$TMP/moment-update.qui" <<'QUI'
class Model
    neural.Parameter<float32> left
    neural.Parameter<float32> right

class MomentState
    float rate
    float beta1
    float beta2
    float epsilon
    neural.State<int> step
    neural.State<bin> moments

Model model = Model(
    left = neural.Parameter<float32>(value = tensor.ones<float32>([1])),
    right = neural.Parameter<float32>(value = tensor.ones<float32>([1]) * 2.0)
)
MomentState state = MomentState(
    rate = 0.1,
    beta1 = 0.9,
    beta2 = 0.999,
    epsilon = 0.00000001,
    step = neural.State<int>(value = 0),
    moments = neural.State<bin>(value = bin.fill(0, 0))
)
neural<float32> left = model.left.track()
neural<float32> right = model.right.track()
neural<float32> loss = neural.mean(left * left) + neural.mean(right * right)
neural.Gradients gradients = neural.grad(loss)
neural.moment_update(
    &model,
    state.rate,
    state.beta1,
    state.beta2,
    state.epsilon,
    &state.step,
    &state.moments,
    gradients
)
print(state.step.value)
print(math.abs(float(model.left.raw()[0].item()) - 0.9) < 0.000001)
print(math.abs(float(model.right.raw()[0].item()) - 1.9) < 0.000001)

neural<float32> left_second = model.left.track()
neural<float32> right_second = model.right.track()
neural<float32> loss_second = neural.mean(left_second * left_second) + neural.mean(right_second * right_second)
neural.Gradients gradients_second = neural.grad(loss_second)
neural.moment_update(
    &model,
    state.rate,
    state.beta1,
    state.beta2,
    state.epsilon,
    &state.step,
    &state.moments,
    gradients_second
)
print(state.step.value)
print(model.left.raw()[0].item() < float32(0.9))
print(model.right.raw()[0].item() < float32(1.9))
QUI

moment_output="$("$QUIDRA" "$TMP/moment-update.qui")"
if [[ "$moment_output" != "$(printf '1\ntrue\ntrue\n2\ntrue\ntrue')" ]]; then
    echo "unexpected neural moment update output: $moment_output" >&2
    exit 1
fi

cat > "$TMP/convolve.qui" <<'QUI'
class Model
    neural.Parameter<float32> weight
    neural.Parameter<float32> bias

tensor<float32> identity = tensor.zeros<float32>([1, 1, 2, 2])
identity[0, 0, 0, 0] = 1.0
identity[0, 0, 1, 1] = 1.0
Model model = Model(
    weight = neural.Parameter<float32>(value = identity),
    bias = neural.Parameter<float32>(value = tensor.zeros<float32>([1]))
)

tensor<float32> samples = tensor.zeros<float32>([1, 1, 3, 3])
int value = 1
for y in range(3)
    for x in range(3)
        samples[0, 0, y, x] = float32(value)
        value += 1

neural<float32> output = neural.convolve2d(
    neural.track(samples), model.weight, model.bias, 1, 0
)
tensor<float32> plain = output.untrack()
print(plain.shape()[2])
print(plain.shape()[3])
print(plain[0, 0, 0, 0].item())
print(plain[0, 0, 0, 1].item())
print(plain[0, 0, 1, 0].item())
print(plain[0, 0, 1, 1].item())

tensor<float32> padded = neural.convolve2d(
    samples, model.weight, model.bias, 1, 1
)
print(padded.shape()[2])
print(padded.shape()[3])

neural<float32> loss = neural.mean(output)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 1.0)
print(model.bias.raw()[0].item())
QUI

convolve_output="$("$QUIDRA" "$TMP/convolve.qui")"
convolve_expected="$(printf '2\n2\n6.0\n8.0\n12.0\n14.0\n4\n4\n-1.0')"
if [[ "$convolve_output" != "$convolve_expected" ]]; then
    echo "unexpected neural convolution output: $convolve_output" >&2
    exit 1
fi

cat > "$TMP/convolve-coverage.qui" <<'QUI'
class ConvCoverage
    neural.Parameter<float32> weight
    neural.Parameter<float32> bias

ConvCoverage pointwise = ConvCoverage(
    weight = neural.Parameter<float32>(
        value = tensor.ones<float32>([3, 2, 1, 1])
    ),
    bias = neural.Parameter<float32>(
        value = tensor.ones<float32>([3])
    )
)
tensor<float32> batch = tensor.ones<float32>([2, 2, 3, 5])
tensor<float32> point = neural.convolve2d(
    batch, pointwise.weight, pointwise.bias, 1, 0
)
print(point.shape()[0])
print(point.shape()[1])
print(point.shape()[2])
print(point.shape()[3])
print(point[1, 2, 2, 4].item())

tensor<float32> point_stride = neural.convolve2d(
    batch, pointwise.weight, pointwise.bias, 2, 0
)
print(point_stride.shape()[2])
print(point_stride.shape()[3])
print(point_stride[1, 1, 1, 2].item())

ConvCoverage three = ConvCoverage(
    weight = neural.Parameter<float32>(
        value = tensor.ones<float32>([3, 2, 3, 3])
    ),
    bias = neural.Parameter<float32>(
        value = tensor.ones<float32>([3])
    )
)
tensor<float32> three_plain = neural.convolve2d(
    tensor.ones<float32>([2, 2, 5, 7]),
    three.weight, three.bias, 1, 0
)
print(three_plain.shape()[2])
print(three_plain.shape()[3])
print(three_plain[1, 2, 2, 4].item())

tensor<float32> three_padded = neural.convolve2d(
    tensor.ones<float32>([2, 2, 5, 7]),
    three.weight, three.bias, 1, 1
)
print(three_padded.shape()[2])
print(three_padded.shape()[3])
print(three_padded[0, 0, 0, 0].item())
print(three_padded[0, 0, 2, 3].item())

tensor<float32> three_stride = neural.convolve2d(
    tensor.ones<float32>([2, 2, 5, 7]),
    three.weight, three.bias, 2, 1
)
print(three_stride.shape()[2])
print(three_stride.shape()[3])
print(three_stride[0, 0, 0, 0].item())
print(three_stride[0, 0, 1, 1].item())

ConvCoverage five = ConvCoverage(
    weight = neural.Parameter<float32>(
        value = tensor.ones<float32>([1, 1, 5, 5])
    ),
    bias = neural.Parameter<float32>(
        value = tensor.ones<float32>([1])
    )
)
tensor<float32> five_out = neural.convolve2d(
    tensor.ones<float32>([1, 1, 6, 8]),
    five.weight, five.bias, 1, 0
)
print(five_out.shape()[2])
print(five_out.shape()[3])
print(five_out[0, 0, 1, 3].item())

class ConvBackward
    neural.Parameter<float32> pixels
    neural.Parameter<float32> weight
    neural.Parameter<float32> bias

ConvBackward backward = ConvBackward(
    pixels = neural.Parameter<float32>(
        value = tensor.ones<float32>([1, 1, 3, 3])
    ),
    weight = neural.Parameter<float32>(
        value = tensor.ones<float32>([1, 1, 3, 3])
    ),
    bias = neural.Parameter<float32>(
        value = tensor.zeros<float32>([1])
    )
)
neural<float32> tracked_out = neural.convolve2d(
    backward.pixels.track(), backward.weight, backward.bias, 1, 0
)
neural.Gradients grads = neural.grad(neural.mean(tracked_out))
neural.update(&backward, grads, rate = 1.0)
print(backward.pixels.raw()[0, 0, 1, 1].item())
print(backward.weight.raw()[0, 0, 2, 2].item())
print(backward.bias.raw()[0].item())

class PointBackward
    neural.Parameter<float32> pixels
    neural.Parameter<float32> weight
    neural.Parameter<float32> bias

PointBackward point_backward = PointBackward(
    pixels = neural.Parameter<float32>(
        value = tensor.ones<float32>([1, 2, 2, 2])
    ),
    weight = neural.Parameter<float32>(
        value = tensor.ones<float32>([1, 2, 1, 1])
    ),
    bias = neural.Parameter<float32>(
        value = tensor.zeros<float32>([1])
    )
)
neural<float32> point_tracked = neural.convolve2d(
    point_backward.pixels.track(),
    point_backward.weight,
    point_backward.bias,
    1,
    0
)
neural.Gradients point_grads = neural.grad(neural.mean(point_tracked))
neural.update(&point_backward, point_grads, rate = 1.0)
print(point_backward.pixels.raw()[0, 1, 1, 1].item())
print(point_backward.weight.raw()[0, 1, 0, 0].item())
print(point_backward.bias.raw()[0].item())

class Conv64
    neural.Parameter<float> weight
    neural.Parameter<float> bias

Conv64 wide = Conv64(
    weight = neural.Parameter<float>(
        value = tensor.ones<float>([1, 1, 3, 3])
    ),
    bias = neural.Parameter<float>(
        value = tensor.ones<float>([1])
    )
)
tensor<float> wide_out = neural.convolve2d(
    tensor.ones<float>([1, 1, 3, 3]),
    wide.weight, wide.bias, 1, 0
)
print(wide_out[0, 0, 0, 0].item())
QUI

coverage_output="$("$QUIDRA" "$TMP/convolve-coverage.qui")"
coverage_expected="$(printf '2\n3\n3\n5\n3.0\n2\n3\n3.0\n3\n5\n19.0\n5\n7\n9.0\n19.0\n3\n4\n9.0\n19.0\n2\n4\n26.0\n0.0\n0.0\n-1.0\n0.75\n0.0\n-1.0\n10.0')"
if [[ "$coverage_output" != "$coverage_expected" ]]; then
    echo "unexpected convolution coverage output:" >&2
    printf '%s\n' "$coverage_output" >&2
    exit 1
fi

cat > "$TMP/persistence.qui" <<QUI
class Model
    neural.Parameter<float32> weight
    neural.State<int> steps

Model model = Model(
    weight = neural.Parameter<float32>(value = tensor.ones<float32>([2])),
    steps = neural.State<int>(value = 3)
)
neural.save(model, path = "$TMP/foundation.quistate")

neural<float32> prediction = model.weight.track()
neural<float32> loss = neural.mean(prediction * prediction)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.5)
model.steps.value = 9
print(model.weight.raw()[0].item() != float32(1))
print(model.steps.value)

neural.load(&model = &model, path = "$TMP/foundation.quistate")
print(model.weight.raw()[0].item() == float32(1))
print(model.steps.value)
QUI

persistence_output="$("$QUIDRA" "$TMP/persistence.qui")"
if [[ "$persistence_output" != "$(printf 'true\n9\ntrue\n3')" ]]; then
    echo "unexpected neural persistence output: $persistence_output" >&2
    exit 1
fi

cat > "$TMP/persistence-mismatch.qui" <<QUI
class Model
    neural.Parameter<float32> weight
    neural.State<int> steps

Model other = Model(
    weight = neural.Parameter<float32>(value = tensor.ones<float32>([5])),
    steps = neural.State<int>(value = 0)
)
neural.load(&model = &other, path = "$TMP/foundation.quistate")
print(other.weight.raw()[0].item())
QUI

set +e
"$QUIDRA" "$TMP/persistence-mismatch.qui" >"$TMP/persistence-mismatch.out" 2>&1
mismatch_rc=$?
set -e
if [[ "$mismatch_rc" -eq 0 ]]; then
    echo "a mismatched .quistate shape was accepted" >&2
    exit 1
fi
grep -q 'NEURAL_STATE' "$TMP/persistence-mismatch.out"

cat > "$TMP/primitive-inference.qui" <<'QUI'
X identity<X>(X value)
    return value

tensor<float32> values = tensor.ones<float32>([1, 3])
neural<float32> tracked = neural.track(values)
print(identity(neural.absolute(tracked)).untrack().shape()[1])
print(identity(neural.exponential(tracked)).untrack().shape()[1])
print(identity(neural.logarithm(neural.absolute(tracked))).untrack().shape()[1])
print(identity(neural.sum_last(tracked)).untrack().shape()[1])
print(identity(neural.max_last(tracked)).untrack().shape()[1])
QUI

inference_output="$("$QUIDRA" "$TMP/primitive-inference.qui")"
if [[ "$inference_output" != "$(printf '3\n3\n3\n3\n3')" ]]; then
    echo "unexpected neural primitive inference output: $inference_output" >&2
    exit 1
fi

cat > "$TMP/random-mask-rate.qui" <<'QUI'
neural.State<uint64> rng = neural.State<uint64>(value = uint64(7))
tensor<float32> values = tensor.ones<float32>([4])
neural<float32> masked = neural.random_mask(neural.track(values), rng, rate = 1.0)
print(masked.untrack()[0].item())
QUI

set +e
"$QUIDRA" "$TMP/random-mask-rate.qui" >"$TMP/random-mask-rate.out" 2>&1
rate_rc=$?
set -e
if [[ "$rate_rc" -eq 0 ]]; then
    echo "an out-of-range random_mask rate was accepted" >&2
    exit 1
fi
grep -q 'rate must be in \[0,1)' "$TMP/random-mask-rate.out"

cat > "$TMP/state-write-effect.qui" <<'QUI'
class Masked
    float rate
    neural.State<uint64> rng

    neural<float32> forward(neural<float32> value)
        return neural.random_mask(value, rng, rate = rate)

void observe(const Masked &subject, neural<float32> value)
    auto ignored = subject.forward(value)

Masked subject = Masked(
    rate = 0.5, rng = neural.State<uint64>(value = uint64(7))
)
observe(&subject, neural.track(tensor.ones<float32>([4])))
QUI

set +e
"$QUIDRA" check "$TMP/state-write-effect.qui" --json >"$TMP/state-write-effect.json" 2>&1
state_rc=$?
set -e
if [[ "$state_rc" -ne 1 ]]; then
    echo "State mutation through a const access path was accepted" >&2
    exit 1
fi
grep -q 'WRITE_CAPABILITY' "$TMP/state-write-effect.json"

cat > "$TMP/normalize-write-effect.qui" <<'QUI'
class Normalized
    neural.Parameter<float32> scale
    neural.Parameter<float32> bias
    neural.State<tensor<float32>> running_mean
    neural.State<tensor<float32>> running_variance

    neural<float32> forward(neural<float32> value)
        return neural.normalize(
            value, scale, bias, running_mean, running_variance,
            momentum = 0.5, epsilon = 0.00001
        )

void observe(const Normalized &subject, neural<float32> value)
    auto ignored = subject.forward(value)

Normalized subject = Normalized(
    scale = neural.Parameter<float32>(value = tensor.ones<float32>([2])),
    bias = neural.Parameter<float32>(value = tensor.zeros<float32>([2])),
    running_mean = neural.State<tensor<float32>>(
        value = tensor.zeros<float32>([2])
    ),
    running_variance = neural.State<tensor<float32>>(
        value = tensor.ones<float32>([2])
    )
)
observe(&subject, neural.track(tensor.ones<float32>([2, 2])))
QUI

set +e
"$QUIDRA" check "$TMP/normalize-write-effect.qui" --json >"$TMP/normalize-write-effect.json" 2>&1
normalize_rc=$?
set -e
if [[ "$normalize_rc" -ne 1 ]]; then
    echo "running state mutation through a const access path was accepted" >&2
    exit 1
fi
grep -q 'WRITE_CAPABILITY' "$TMP/normalize-write-effect.json"

for guarded in "p.value = tensor.zeros<float32>([2])" "p.value[0] = 5.0"; do
    cat > "$TMP/parameter-write.qui" <<QUI
neural.Parameter<float32> p = neural.Parameter<float32>(
    value = tensor.ones<float32>([2])
)
$guarded
QUI
    set +e
    "$QUIDRA" check "$TMP/parameter-write.qui" --json >"$TMP/parameter-write.json" 2>&1
    parameter_rc=$?
    set -e
    if [[ "$parameter_rc" -ne 1 ]]; then
        echo "Parameter storage accepted a direct write: $guarded" >&2
        exit 1
    fi
    grep -q 'WRITE_CAPABILITY' "$TMP/parameter-write.json"
done

cat > "$TMP/parameter-reference.qui" <<'QUI'
void overwrite(tensor<float32> &values)
    values[0] = 1.0

neural.Parameter<float32> p = neural.Parameter<float32>(
    value = tensor.ones<float32>([2])
)
overwrite(&p.value)
QUI

set +e
"$QUIDRA" check "$TMP/parameter-reference.qui" --json >"$TMP/parameter-reference.json" 2>&1
parameter_reference_rc=$?
set -e
if [[ "$parameter_reference_rc" -ne 1 ]]; then
    echo "Parameter storage was exposed through a writable reference" >&2
    exit 1
fi
grep -q 'WRITE_CAPABILITY' "$TMP/parameter-reference.json"

for guarded in \
    "momentum = 5.0, epsilon = 0.00001" \
    "momentum = 0.0 - 2.0, epsilon = 0.00001" \
    "momentum = 0.1, epsilon = 0.0 - 1.0"; do
    cat > "$TMP/normalize-range.qui" <<QUI
neural.Parameter<float32> scale = neural.Parameter<float32>(
    value = tensor.ones<float32>([2])
)
neural.Parameter<float32> bias = neural.Parameter<float32>(
    value = tensor.zeros<float32>([2])
)
neural.State<tensor<float32>> running_mean = neural.State<tensor<float32>>(
    value = tensor.zeros<float32>([2])
)
neural.State<tensor<float32>> running_variance = neural.State<tensor<float32>>(
    value = tensor.ones<float32>([2])
)
neural<float32> out = neural.normalize(
    neural.track(tensor.ones<float32>([2, 2])),
    scale, bias, running_mean, running_variance, $guarded
)
print(out.untrack()[0, 0].item())
QUI
    set +e
    "$QUIDRA" "$TMP/normalize-range.qui" >"$TMP/normalize-range.out" 2>&1
    normalize_range_rc=$?
    set -e
    if [[ "$normalize_range_rc" -eq 0 ]]; then
        echo "neural.normalize accepted out-of-range operands: $guarded" >&2
        exit 1
    fi
    grep -q 'must be' "$TMP/normalize-range.out"
done

for removed in Linear Conv2D BatchNorm Dropout SGD Adam relu sigmoid tanh \
    softmax gelu mse cross_entropy binary_cross_entropy step training inference; do
    printf 'auto removed = neural.%s\n' "$removed" > "$TMP/removed-api.qui"
    set +e
    "$QUIDRA" check "$TMP/removed-api.qui" >/dev/null 2>&1
    removed_rc=$?
    set -e
    if [[ "$removed_rc" -eq 0 ]]; then
        echo "neural.$removed is still exposed by Quidra core" >&2
        exit 1
    fi
done

cat > "$TMP/high-level-core-api.qui" <<'QUI'
neural.Linear layer
neural.SGD optimizer
neural<float32> value = neural.relu(
    neural.track(tensor.ones<float32>([1]))
)
QUI

set +e
"$QUIDRA" check "$TMP/high-level-core-api.qui" --json >"$TMP/high-level-core-api.json"
high_level_rc=$?
set -e
if [[ "$high_level_rc" -ne 1 ]]; then
    echo "high-level neural API is still exposed by Quidra core" >&2
    exit 1
fi
grep -Eq 'UNKNOWN_TYPE|UNKNOWN_MODULE_MEMBER' "$TMP/high-level-core-api.json"

echo "neural foundation integration: ok"
