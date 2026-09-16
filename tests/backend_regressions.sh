#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
OPT="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

verify_and_run() {
    local name="$1"
    local expected="$2"
    local source="$TMP/$name.qui"
    local llvm="$TMP/$name.ll"

    "$QUIDRA" llvm "$source" > "$llvm"
    "$OPT" -passes=verify -disable-output "$llvm"

    local actual
    actual="$("$QUIDRA" run "$source")"
    if [[ "$actual" != "$expected" ]]; then
        printf 'backend regression %s failed\nexpected:\n%s\nactual:\n%s\n' \
            "$name" "$expected" "$actual" >&2
        exit 1
    fi
}

cat > "$TMP/short-circuit.qui" <<'QUI'
int a = 4
if a % 2 == 0 and a % 4 == 0
    print("mod")

if a + 1 == 5 and 12 / a == 3
    print("arith")

if a == 4 or 10 / a == 0
    print("or")
QUI
verify_and_run short-circuit "$(printf 'mod\narith\nor')"

cat > "$TMP/contextual-literals.qui" <<'QUI'
float x = 3
float32 y = 2
float[] xs = [1, 2, 3]

float half(float value)
    return value / 2.0

float negative = -3
float mixed = 2.0 * 3
int8 small = 5
int8 sum = small + 100

print(x)
print(y)
print(xs[2])
print(half(3))
print(negative)
print(mixed)
print(sum)
QUI
verify_and_run contextual-literals "$(printf '3.0\n2.0\n3.0\n1.5\n-3.0\n6.0\n105')"

cat > "$TMP/try-class.qui" <<'QUI'
class Pair
    int a
    int b

Pair | error make(bool ok)
    if ok
        return Pair(a = 2, b = 3)
    return error("bad")

int | error sum(bool ok)
    Pair pair = try make(ok)
    return pair.a + pair.b

auto result = sum(true)
match result
    int value
        print(value)
    error problem
        print(problem)
QUI
verify_and_run try-class "5"


cat > "$TMP/step-prevalidation.qui" <<'QUI'
class StepModel
    neural.Parameter value

StepModel model = StepModel(
    value = neural.Parameter(value = tensor.ones<float32>([1]))
)
neural prediction = model.value.track()
neural loss = neural.mse(prediction, tensor.zeros<float32>([1]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.1)
neural.step(&model, &optimizer, gradients)
print(model.value.raw()[0].item())
QUI
"$QUIDRA" llvm "$TMP/step-prevalidation.qui" > "$TMP/step-prevalidation.ll"
"$OPT" -passes=verify -disable-output "$TMP/step-prevalidation.ll"
probe_line="$(grep -n 'call i1 @quidra_neural_parameter_has_gradient' "$TMP/step-prevalidation.ll" | head -n1 | cut -d: -f1)"
validate_line="$(grep -n 'call void @quidra_neural_validate_step' "$TMP/step-prevalidation.ll" | head -n1 | cut -d: -f1)"
update_line="$(grep -n 'call i1 @quidra_neural_sgd_step_parameter' "$TMP/step-prevalidation.ll" | head -n1 | cut -d: -f1)"
[[ -n "$probe_line" && -n "$validate_line" && -n "$update_line" ]]
[[ "$probe_line" -lt "$validate_line" && "$validate_line" -lt "$update_line" ]]
[[ "$("$QUIDRA" run "$TMP/step-prevalidation.qui")" == "0.800000011920929" ]]


cat > "$TMP/adam-step-prevalidation.qui" <<'QUI'
class AdamStepModel
    neural.Linear first
    neural.Linear second

AdamStepModel model = AdamStepModel(
    first = neural.Linear(input = 2, output = 2, seed = 11),
    second = neural.Linear(input = 2, output = 1, seed = 13)
)
tensor<float32> values = tensor.ones<float32>([1, 2])
neural prediction = model.second.forward(model.first.forward(neural.track(values)))
neural loss = neural.mse(prediction, tensor.zeros<float32>([1, 1]))
neural.Gradients gradients = neural.grad(loss)
neural.Adam optimizer = neural.Adam(rate = 0.01)
neural.step(&model, &optimizer, gradients)
QUI
"$QUIDRA" llvm "$TMP/adam-step-prevalidation.qui" > "$TMP/adam-step-prevalidation.ll"
"$OPT" -passes=verify -disable-output "$TMP/adam-step-prevalidation.ll"
adam_begin_line="$(grep -n 'call i64 @quidra_neural_adam_begin' "$TMP/adam-step-prevalidation.ll" | head -n1 | cut -d: -f1)"
adam_first_validate_line="$(grep -n 'call void @quidra_neural_adam_validate_parameter' "$TMP/adam-step-prevalidation.ll" | head -n1 | cut -d: -f1)"
adam_last_validate_line="$(grep -n 'call void @quidra_neural_adam_validate_parameter' "$TMP/adam-step-prevalidation.ll" | tail -n1 | cut -d: -f1)"
adam_first_update_line="$(grep -n 'call i1 @quidra_neural_adam_step_parameter' "$TMP/adam-step-prevalidation.ll" | head -n1 | cut -d: -f1)"
adam_finish_line="$(grep -n 'call void @quidra_neural_adam_finish' "$TMP/adam-step-prevalidation.ll" | head -n1 | cut -d: -f1)"
[[ -n "$adam_begin_line" && -n "$adam_first_validate_line" && -n "$adam_last_validate_line" && -n "$adam_first_update_line" && -n "$adam_finish_line" ]]
[[ "$adam_begin_line" -lt "$adam_first_validate_line" && "$adam_last_validate_line" -lt "$adam_first_update_line" && "$adam_first_update_line" -lt "$adam_finish_line" ]]
