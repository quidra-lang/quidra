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

cat > "$TMP/moment-update.qui" <<'QUI'
class Model
    neural.Parameter<float32> value

class MomentState
    float rate
    float beta1
    float beta2
    float epsilon
    neural.State<int> step
    neural.State<bytes> moments

Model model = Model(
    value = neural.Parameter<float32>(value = tensor.ones<float32>([1]))
)
MomentState state = MomentState(
    rate = 0.1,
    beta1 = 0.9,
    beta2 = 0.999,
    epsilon = 0.00000001,
    step = neural.State<int>(value = 0),
    moments = neural.State<bytes>(value = bytes())
)
neural<float32> tracked = model.value.track()
neural<float32> loss = neural.mean(tracked * tracked)
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
print(math.abs(float(model.value.raw()[0].item()) - 0.9) < 0.000001)
QUI

moment_output="$("$QUIDRA" "$TMP/moment-update.qui")"
if [[ "$moment_output" != "$(printf '1\ntrue')" ]]; then
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
    "momentum = 0 - 2.0, epsilon = 0.00001" \
    "momentum = 0.1, epsilon = 0 - 1.0"; do
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
