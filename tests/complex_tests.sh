#!/usr/bin/env bash
# The complex types com16, com16b, com32, com64 and com: literals, parts,
# arithmetic with its rounding and checks, equality and the lack of an
# ordering, conversions, text and parsing, exact com values and CPU tensors.
set -euo pipefail
set -x
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"

# fails NAME CODE MESSAGE LOCATION: $TMP/NAME.qui stops with status 101.
fails() {
    local name="$1" code="$2" message="$3" location="$4"
    local rc=0
    "$QUIDRA" run "$TMP/$name.qui" >"$TMP/$name.out" 2>"$TMP/$name.err" || rc=$?
    [[ "$rc" -eq 101 ]]
    grep -q "Quidra runtime error\[$code\] at .*$name.qui:$location\$" "$TMP/$name.err"
    grep -q "| $message\$" "$TMP/$name.err"
}

# rejects NAME CODE MESSAGE: $TMP/NAME.qui does not compile.
rejects() {
    local name="$1" code="$2" message="$3"
    if "$QUIDRA" check "$TMP/$name.qui" >"$TMP/$name.err" 2>&1; then exit 1; fi
    grep -q "error\[$code\]" "$TMP/$name.err"
    grep -Fq "$message" "$TMP/$name.err"
}

cat > "$TMP/literals.qui" <<'QUI'
com64 z = 1.0 + 2.0i
com32 w = 1.5 - 0.25i
com64 j = 2.0i
com16 h = 0.1 + 0.1i
com16b b = 1.0 - 3.0i
com32 negative = -1.0 - 0.0i
print("{z} {w} {j} {h} {b} {negative}")
print(NL)
print("{z.real} {z.imag} {w.imag} {h.real}")
print(NL)
print(z.string())
print(NL)
print("{w:frac=2}")
print(NL)
QUI
"$QUIDRA" run "$TMP/literals.qui" >"$TMP/literals.out"
expected_literals=$(cat <<'TXT'
1.0+2.0i 1.5-0.25i 0.0+2.0i 0.1+0.1i 1.0-3.0i -1.0-0.0i
1.0 2.0 -0.25 0.1
1.0+2.0i
1.50-0.25i
TXT
)
[[ "$(cat "$TMP/literals.out")" == "$expected_literals" ]]

# Arithmetic in the part type, each operation rounded once; Smith's
# division.
cat > "$TMP/arithmetic.qui" <<'QUI'
com64 a = 1.0 + 2.0i
com64 c = 3.0 - 1.0i
print("{a + c} {a - c} {a * c} {a / c} {-a}")
print(NL)
com32 x = 1.5 - 0.25i
com32 y = 0.5 + 2.0i
print("{x / y} {x * y}")
print(NL)
print("{a == a} {a != c} {a == (1.0 + 2.0i)}")
print(NL)
com16 i = 1.0i
print(i ^ (2.0 + 0.0i))
print(NL)
com16b big = 3.0 + 0.0i
print(big * big)
print(NL)
QUI
"$QUIDRA" run "$TMP/arithmetic.qui" >"$TMP/arithmetic.out"
expected_arithmetic=$(cat <<'TXT'
4.0+1.0i -2.0+3.0i 5.0+5.0i 0.1+0.7000000000000001i -1.0-2.0i
0.05882353-0.7352941i 1.25+2.875i
true true true
-1.0+0.0i
9.0+0.0i
TXT
)
[[ "$(cat "$TMP/arithmetic.out")" == "$expected_arithmetic" ]]

# Failures name the complex type.
cat > "$TMP/overflow.qui" <<'QUI'
com32 big = 3.0e38 + 0.0i
com32 two = 2.0 + 0.0i
print(big * two)
QUI
[[ "$("$QUIDRA" run "$TMP/overflow.qui")" == "inf+0.0i" ]]
cat > "$TMP/nan.qui" <<'QUI'
com32 large = 3.0e38 + 0.0i
com32 infinite = large + large
print(infinite - infinite)
QUI
fails nan NONFINITE_RESULT 'com32 subtraction produced NaN' '3:7'
cat > "$TMP/divide.qui" <<'QUI'
com64 one = 1.0 + 1.0i
com64 zero = 0.0 + 0.0i
print(one / zero)
QUI
fails divide DIVISION_BY_ZERO 'com64 division by zero' '3:7'
cat > "$TMP/power.qui" <<'QUI'
com64 zero = 0.0i
com64 exponent = 0.0i
print(zero ^ exponent)
QUI
fails power POWER_DOMAIN '0 ^ 0 is undefined' '3:7'

# Conversions: explicit, a scalar is the real part; complex types part by
# part, range-checked when narrowing.
cat > "$TMP/conversions.qui" <<'QUI'
real32 x = 2.5
int32 n = -3
com32 a = com32(x)
com64 b = com64(n)
com64 c = com64(1.0)
com16 d = com16(a)
com64 | error narrowed = com64(1.0e300 + 1.0i)
com32 | error fits = com32(com64(0.5 - 1.5i))
com32 | error beyond = com32(com64(1.0e300 + 0.0i))
print("{a} {b} {c} {d}")
print(NL)
match fits
    com32 value
        print(value)
    error problem
        print(problem)
print(NL)
match beyond
    com32 value
        print(value)
    error problem
        print(problem)
print(NL)
QUI
"$QUIDRA" run "$TMP/conversions.qui" >"$TMP/conversions.out"
expected_conversions=$(cat <<'TXT'
2.5+0.0i -3.0+0.0i 1.0+0.0i 2.5+0.0i
0.5-1.5i
numeric conversion out of range: value cannot be represented as com32
TXT
)
[[ "$(cat "$TMP/conversions.out")" == "$expected_conversions" ]]

# Parsing reads the printed form.
cat > "$TMP/parse.qui" <<'QUI'
string[] texts = ["1.5-0.25i", "-2.0+1.0e2i", "3.0", "1.0+i", "1.0e39+0.0i"]
for auto text = texts[]
    com32 | error value = com32.parse(text)
    match value
        com32 parsed
            print(parsed)
        error problem
            print("error")
    print(NL)
com | error exact = com.parse("0.25-0.5i")
match exact
    com parsed
        print(parsed.imag)
    error problem
        print("error")
print(NL)
QUI
expected_parse=$(cat <<'TXT'
1.5-0.25i
-2.0+100.0i
error
error
error
-0.5
TXT
)
[[ "$("$QUIDRA" run "$TMP/parse.qui")" == "$expected_parse" ]]

# Exact com values.
cat > "$TMP/exact.qui" <<'QUI'
com x = 0.5 + 0.25i
com y = x * x
print("{y} {x - x} {x / (0.0 + 1.0i)} {x == (0.5 + 0.25i)}")
print(NL)
real r = y.real
print(r)
print(NL)
com[] values = [x, y]
print(values[1] == y)
print(NL)
QUI
expected_exact=$(cat <<'TXT'
0.1875+0.25i 0.0+0.0i 0.25-0.5i true
0.1875
true
TXT
)
[[ "$("$QUIDRA" run "$TMP/exact.qui")" == "$expected_exact" ]]
cat > "$TMP/exact_divide.qui" <<'QUI'
com x = 0.5 + 0.25i
com zero = 0.0i
print(x / zero)
QUI
fails exact_divide DIVISION_BY_ZERO 'com division by zero' '3:7'

# CPU tensors.
cat > "$TMP/tensors.qui" <<'QUI'
tensor<com32> t = tensor.ones<com32>([2]) * (2.0 + 1.0i)
tensor<com32> u = t * t / (1.0 - 1.0i)
print("{t[0].item()} {u[1].item()} {(-t)[0].item()}")
print(NL)
tensor<real32> real_values = tensor.ones<real32>([2]) * real32(0.5)
tensor<com64> widened = com64(real_values)
print(widened[0].item())
print(NL)
tensor<bool> same = t == t
print(same[1].item())
print(NL)
QUI
"$QUIDRA" run "$TMP/tensors.qui" >"$TMP/tensors.out"
[[ "$(cat "$TMP/tensors.out")" == "$(printf '2.0+1.0i -0.5+3.5i -2.0-1.0i\n0.5+0.0i\ntrue')" ]]
cat > "$TMP/tensor_overflow.qui" <<'QUI'
tensor<com32> big = tensor.ones<com32>([2]) * (3.0e38 + 0.0i)
tensor<com32> twice = big + big
print(twice[0].item())
QUI
[[ "$("$QUIDRA" run "$TMP/tensor_overflow.qui")" == "inf+0.0i" ]]
cat > "$TMP/tensor_nan.qui" <<'QUI'
tensor<com32> big = tensor.ones<com32>([2]) * (3.0e38 + 0.0i)
tensor<com32> infinite = big + big
tensor<com32> invalid = infinite - infinite
QUI
fails tensor_nan NONFINITE_RESULT 'tensor<com32> subtraction produced NaN' '3:25'

# Compile-time errors.
cat > "$TMP/ordering.qui" <<'QUI'
com64 a = 1.0i
com64 b = 2.0i
bool less = a < b
QUI
rejects ordering TYPE_MISMATCH 'Complex values have no ordering.'
cat > "$TMP/mixing.qui" <<'QUI'
real32 x = 1.0
com32 z = 1.0i
com32 sum = x + z
QUI
rejects mixing TYPE_MISMATCH 'real32 and com32 do not mix; convert explicitly with com32(x).'
cat > "$TMP/real_literal.qui" <<'QUI'
com32 z = 1.0
QUI
rejects real_literal NUMERIC_FAMILY 'A real literal cannot materialize as com32: write 1.0 + 0.0i or com32(1.0).'
cat > "$TMP/to_real.qui" <<'QUI'
com32 z = 1.0i
real32 x = real32(z)
QUI
rejects to_real NUMERIC_CAST 'A complex value has no direct real32 conversion: use z.real or math.abs(z).'
cat > "$TMP/sorted.qui" <<'QUI'
com64[] values = [1.0i, 2.0i]
com64[] ordered = values.sorted()
QUI
rejects sorted TYPE_MISMATCH 'Complex values have no ordering.'
cat > "$TMP/integer_form.qui" <<'QUI'
com64 z = 2i
QUI
rejects integer_form NUMERIC_FAMILY 'An imaginary literal requires real form: write 2.0i.'
cat > "$TMP/exact_power.qui" <<'QUI'
com z = 1.0i
com w = z ^ z
QUI
rejects exact_power TYPE_MISMATCH 'Exponentiation of com values is not available'
cat > "$TMP/part_assignment.qui" <<'QUI'
com64 z = 1.0i
z.real = 2.0
QUI
if "$QUIDRA" check "$TMP/part_assignment.qui" >/dev/null 2>&1; then exit 1; fi

# A com64 gather copies both binary64 parts of each selected element.
cat > "$TMP/gather_com64.qui" <<'QUI'
tensor<com64> values = tensor.ones<com64>([2])
values[0] = com64(1.0 - 0.0i)
values[1] = com64(-0.0 + 2.0i)
tensor<com64> result = (-values).gather([1, 0], [2])
print("{result[0].item()} {result[1].item()}")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/gather_com64.qui")" == '0.0-2.0i -1.0+0.0i' ]]
