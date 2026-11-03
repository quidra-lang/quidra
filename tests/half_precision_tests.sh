#!/usr/bin/env bash
# The 16-bit reals real16 (IEEE binary16) and real16b (bfloat16): literals
# rounded once, arithmetic computed exactly and rounded once per operation,
# the shortest text that reads back per format, parsing, conversions with
# their range checks, infinity and NaN checks, arrays, CPU tensors and their
# gradients.
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

cat > "$TMP/scalars.qui" <<'QUI'
real16 a = 0.1
real16b b = 0.1
print("{a} {b}")
print(NL)
real16 third = 1.0 / 3.0
real16b big = 3.0e38
print("{third} {big} {a + a} {b * b} {-a}")
print(NL)
real16 h = 1.5
real16 one = 1.0
print("{h * h - one} {h / one} {h > one} {h == 1.5} {h ^ 2.0}")
print(NL)
real16b c = 2.0
print("{c ^ 0.5} {c - 3.0} {c != 2.0}")
print(NL)
print("{a:frac=4} {b:sig=3} {h:int=3,frac=1,zero}")
print(NL)
print(a.string())
print(NL)
real16 tiny = 5.96e-8
real16 largest = 65504.0
print("{tiny} {largest} {real16(-0.0)}")
print(NL)
QUI
"$QUIDRA" run "$TMP/scalars.qui" >"$TMP/scalars.out"
expected_scalars=$(cat <<'TXT'
0.1 0.1
0.3333 3.0e+38 0.2 0.01 -0.1
1.25 1.5 true true 2.25
1.414 -1.0 false
0.1000 0.100 001.5
0.1
6.0e-08 65500.0 -0.0
TXT
)
[[ "$(cat "$TMP/scalars.out")" == "$expected_scalars" ]]

# Every operation rounds once: the float result of the exact operands.
cat > "$TMP/rounding.qui" <<'QUI'
real16 x = 1.0
real16 eps = 0.000488
real16 sum = x + eps
print("{sum} {real32(sum)}")
print(NL)
real16b y = 1.0
real16b small = 0.00390625
print("{y + small} {real32(y + small)}")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/rounding.qui")" == "$(printf '1.0 1.0\n1.0 1.0')" ]]

# Conversions: exact widening, rounding and range checks when narrowing.
cat > "$TMP/conversions.qui" <<'QUI'
real16 h = 0.1
real64 wide = real64(h)
real32 single = real32(h)
print("{wide} {single}")
print(NL)
real64 big = 70000.0
real16 | error narrowed = real16(big)
match narrowed
    real16 value
        print(value)
    error problem
        print(problem)
print(NL)
real16b b = real16b(int64(1000001))
int32 n = 300
real16 from_int = real16(nat8(200))
print("{b} {from_int} {real16b(n)}")
print(NL)
real value = real(h)
print(value)
print(NL)
QUI
"$QUIDRA" run "$TMP/conversions.qui" >"$TMP/conversions.out"
[[ "$(sed -n 1p "$TMP/conversions.out")" == "0.0999755859375 0.099975586" ]]
[[ "$(sed -n 2p "$TMP/conversions.out")" == "numeric conversion out of range: value cannot be represented as real16" ]]
[[ "$(sed -n 3p "$TMP/conversions.out")" == "1.0e+06 200.0 300.0" ]]

# Exact values immediately above a half midpoint must not be rounded to
# binary64 first: that would turn them into ties and choose the lower value.
cat > "$TMP/exact-midpoints.qui" <<'QUI'
real half_midpoint = 1.0 + 1.0 / 2048.0
real bfloat_midpoint = 1.0 + 1.0 / 256.0
real epsilon = 1.0 / (2.0 ^ 60.0)
real16 h = real16(half_midpoint + epsilon)
real16b b = real16b(bfloat_midpoint + epsilon)
print("{real64(h)} {real64(b)}")
print(NL)
com exact = com(half_midpoint + epsilon) + com(bfloat_midpoint + epsilon) * 1.0i
com16 hc = com16(exact)
com16b bc = com16b(exact)
print("{real64(hc.real)} {real64(bc.imag)}")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/exact-midpoints.qui")" == "$(printf '1.0009765625 1.0078125\n1.0009765625 1.0078125')" ]]

# Parsing rounds once and rejects text beyond the range.
cat > "$TMP/parse.qui" <<'QUI'
string[] texts = ["0.1", "65504", "65520", "1e-9", "3.0e38", "-2.5"]
for auto text = texts[]
    real16 | error h = real16.parse(text)
    real16b | error b = real16b.parse(text)
    match h
        real16 value
            print(value)
        error problem
            print("error")
    print(" ")
    match b
        real16b value
            print(value)
        error problem
            print("error")
    print(NL)
QUI
expected_parse=$(cat <<'TXT'
0.1 0.1
65500.0 65500.0
error 65500.0
error 1.0e-09
error 3.0e+38
-2.5 -2.5
TXT
)
[[ "$("$QUIDRA" run "$TMP/parse.qui")" == "$expected_parse" ]]

# Overflow and nonzero division by zero produce infinity; NaN remains an error.
cat > "$TMP/overflow16.qui" <<'QUI'
real16 big = 60000.0
real16 two = 2.0
print(big * two)
QUI
[[ "$("$QUIDRA" run "$TMP/overflow16.qui")" == "inf" ]]
cat > "$TMP/divide16b.qui" <<'QUI'
real16b one = 1.0
real16b zero = 0.0
print(one / zero)
QUI
[[ "$("$QUIDRA" run "$TMP/divide16b.qui")" == "inf" ]]
cat > "$TMP/nan16.qui" <<'QUI'
real16 infinity = inf
real16 zero = 0.0
print(infinity * zero)
QUI
fails nan16 NONFINITE_RESULT 'real16 multiplication produced NaN' '3:7'
cat > "$TMP/nan16b.qui" <<'QUI'
real16b zero = 0.0
print(zero / zero)
QUI
fails nan16b DIVISION_BY_ZERO 'real16b division by zero' '2:7'
cat > "$TMP/literal16.qui" <<'QUI'
real16 bad = 70000.0
QUI
if "$QUIDRA" check "$TMP/literal16.qui" >"$TMP/literal16.err" 2>&1; then exit 1; fi
grep -q 'error\[REAL_RANGE\]' "$TMP/literal16.err"

# Arrays sort and compare by value.
cat > "$TMP/arrays.qui" <<'QUI'
real16[] values = [2.5, -1.0, 0.0, -0.0, 0.1]
real16[] ordered = values.sorted()
for auto value = ordered[]
    print("{value} ")
print(NL)
real16b[] b = [3.0, 1.0]
real16b[] c = [3.0, 1.0]
print("{b == c} {b.sorted()[0]}")
print(NL)
for auto value = values.sorted(descending = true)[]
    print("{value} ")
print(NL)
real16b[] signs = [-0.0, 2.0, 0.0, -1.0]
for auto value = signs.sorted(descending = true)[]
    print("{value} ")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/arrays.qui")" == "$(printf -- '-1.0 -0.0 0.0 0.1 2.5 \ntrue 1.0\n2.5 0.1 0.0 -0.0 -1.0 \n2.0 0.0 -0.0 -1.0 ')" ]]

# CPU tensors: creation, arithmetic per element, casts, item, gradients.
cat > "$TMP/tensors.qui" <<'QUI'
tensor<real16> ones = tensor.ones<real16>([3])
tensor<real16> scaled = (ones + ones) * real16(0.1)
print("{scaled[0].item()} {(scaled / real16(3.0))[1].item()}")
print(NL)
tensor<real16b> b = tensor.ones<real16b>([2]) * real16b(0.1)
tensor<real32> widened = real32(b)
print("{b[0].item()} {widened[0].item()}")
print(NL)
tensor<real16> x = (tensor.ones<real16>([1]) * real16(3.0)).track()
tensor<real16> y = x * x
y.backward(&x)
print(x.grad[0].item())
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/tensors.qui")" == "$(printf '0.2 0.06665\n0.1 0.100097656\n6.0')" ]]
cat > "$TMP/tensor_overflow16.qui" <<'QUI'
tensor<real16> big = tensor.ones<real16>([2]) * real16(60000.0)
tensor<real16> twice = big + big
print(twice[0].item())
QUI
[[ "$("$QUIDRA" run "$TMP/tensor_overflow16.qui")" == "inf" ]]
cat > "$TMP/tensor_nan16.qui" <<'QUI'
tensor<real16> infinite = tensor.ones<real16>([2]) * real16(inf)
tensor<real16> invalid = infinite - infinite
QUI
fails tensor_nan16 NONFINITE_RESULT 'tensor<real16> subtraction produced NaN' '2:26'
