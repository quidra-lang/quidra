#!/usr/bin/env bash
set -euxo pipefail

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
float x = float(3)
float32 y = float32(2)
float[] xs = [1.0, 2.0, 3.0]

float half(float value)
    return value / 2.0

float negative = -3.0
float mixed = 2.0 * 3.0
int8 small = 5
int8 sum = small + 100

print(x)
print(y)
print(xs[2])
print(half(3.0))
print(negative)
print(mixed)
print(sum)
QUI
verify_and_run contextual-literals "$(printf '3.0\n2.0\n3.0\n1.5\n-3.0\n6.0\n105')"

cat > "$TMP/float32-rounding.qui" <<'QUI'
float32 rounded = 0.000001
float source = 0.1
float32 narrowed = float32(source)
print(rounded > 0.0000009 and rounded < 0.0000011)
print(narrowed > 0.099 and narrowed < 0.101)
QUI
verify_and_run float32-rounding "$(printf 'true\ntrue')"

cat > "$TMP/float32-range-error.qui" <<'QUI'
float source = 1.0e100
float32 narrowed = float32(source)
print(narrowed)
QUI
set +e
"$QUIDRA" run "$TMP/float32-range-error.qui" >"$TMP/float32-range-error.out" 2>"$TMP/float32-range-error.err"
status=$?
set -e
[[ "$status" -eq 101 ]]
grep -q "NUMERIC_CAST_RANGE" "$TMP/float32-range-error.err"

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
neural loss = neural.mean(prediction * prediction)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.1)
print(model.value.raw()[0].item())
QUI
"$QUIDRA" llvm "$TMP/step-prevalidation.qui" > "$TMP/step-prevalidation.ll"
"$OPT" -passes=verify -disable-output "$TMP/step-prevalidation.ll"
probe_line="$(grep -n 'call i1 @quidra_neural_parameter_has_gradient' "$TMP/step-prevalidation.ll" | head -n1 | cut -d: -f1)"
validate_line="$(grep -n 'call void @quidra_neural_validate_step' "$TMP/step-prevalidation.ll" | head -n1 | cut -d: -f1)"
update_line="$(grep -n 'call i1 @quidra_neural_update_parameter' "$TMP/step-prevalidation.ll" | head -n1 | cut -d: -f1)"
[[ -n "$probe_line" && -n "$validate_line" && -n "$update_line" ]]
[[ "$probe_line" -lt "$validate_line" && "$validate_line" -lt "$update_line" ]]
[[ "$("$QUIDRA" run "$TMP/step-prevalidation.qui")" == "0.800000011920929" ]]


cat > "$TMP/moment-update-prevalidation.qui" <<'QUI'
class MomentUpdateModel
    neural.Parameter<float32> first_weight
    neural.Parameter<float32> first_bias
    neural.Parameter<float32> second_weight
    neural.Parameter<float32> second_bias

MomentUpdateModel model = MomentUpdateModel(
    first_weight = neural.Parameter<float32>(value = tensor.ones<float32>([2, 2])),
    first_bias = neural.Parameter<float32>(value = tensor.zeros<float32>([2])),
    second_weight = neural.Parameter<float32>(value = tensor.ones<float32>([1, 2])),
    second_bias = neural.Parameter<float32>(value = tensor.zeros<float32>([1]))
)
tensor<float32> values = tensor.ones<float32>([1, 2])
neural first = neural.affine(
    neural.track(values), model.first_weight, model.first_bias
)
neural prediction = neural.affine(
    first, model.second_weight, model.second_bias
)
neural loss = neural.mean(prediction * prediction)
neural.Gradients gradients = neural.grad(loss)
neural.State<int> iteration = neural.State<int>(value = 0)
neural.State<bin> moments = neural.State<bin>(value = bin.fill(0, 0))
neural.moment_update(
    &model, 0.01, 0.9, 0.999, 0.00000001,
    &iteration, &moments, gradients
)
QUI
"$QUIDRA" llvm "$TMP/moment-update-prevalidation.qui" > "$TMP/moment-update-prevalidation.ll"
"$OPT" -passes=verify -disable-output "$TMP/moment-update-prevalidation.ll"
moment_begin_line="$(grep -n 'call i64 @quidra_neural_moment_begin' "$TMP/moment-update-prevalidation.ll" | head -n1 | cut -d: -f1)"
moment_first_validate_line="$(grep -n 'call void @quidra_neural_moment_validate_parameter' "$TMP/moment-update-prevalidation.ll" | head -n1 | cut -d: -f1)"
moment_last_validate_line="$(grep -n 'call void @quidra_neural_moment_validate_parameter' "$TMP/moment-update-prevalidation.ll" | tail -n1 | cut -d: -f1)"
moment_first_update_line="$(grep -n 'call i1 @quidra_neural_moment_update_parameter' "$TMP/moment-update-prevalidation.ll" | head -n1 | cut -d: -f1)"
moment_finish_line="$(grep -n 'call void @quidra_neural_moment_finish' "$TMP/moment-update-prevalidation.ll" | head -n1 | cut -d: -f1)"
[[ -n "$moment_begin_line" && -n "$moment_first_validate_line" && -n "$moment_last_validate_line" && -n "$moment_first_update_line" && -n "$moment_finish_line" ]]
[[ "$moment_begin_line" -lt "$moment_first_validate_line" && "$moment_last_validate_line" -lt "$moment_first_update_line" && "$moment_first_update_line" -lt "$moment_finish_line" ]]
