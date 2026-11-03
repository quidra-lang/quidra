#!/usr/bin/env bash
# Signed infinity is valid, but NaN never becomes observable. The first
# indeterminate operation reports NONFINITE_RESULT (DIVISION_BY_ZERO for
# zero/zero), with or without merged checks. Constant expressions enforce
# the same policy. Finite results and parsing range checks are unchanged.
set -euo pipefail
set -x
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"

# fails NAME STATUS CODE MESSAGE LOCATION: runs $TMP/NAME.qui, which must stop
# with STATUS, print the coded header at LOCATION (LINE:COLUMN) and MESSAGE.
fails() {
    local name="$1" status="$2" code="$3" message="$4" location="$5"
    local rc=0
    "$QUIDRA" run "$TMP/$name.qui" >"$TMP/$name.out" 2>"$TMP/$name.err" || rc=$?
    [[ "$rc" -eq "$status" ]]
    grep -q "Quidra runtime error\[$code\] at .*$name.qui:$location\$" "$TMP/$name.err"
    grep -q "| $message\$" "$TMP/$name.err"
}

# Finite results are the IEEE results, as before.
cat > "$TMP/finite.qui" <<'QUI'
real64 x = 2.5
real64 y = -0.125
real32 p = 1.5
real32 q = 0.25
print("{x + y} {x - y} {x * y} {x / y} {x ^ 2.0}")
print(NL)
print("{p + q} {p - q} {p * q} {p / q} {-p}")
print(NL)
real64 tiny = 1.0e-300
print(tiny * 1.0e-300)
print(NL)
real64 zero = 0.0
print(zero / 3.0)
print(NL)
print(-zero * 2.0)
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/finite.qui")" == "$(printf '2.375 2.625 -0.3125 -20.0 6.25\n1.75 1.25 0.375 6.0 -1.5\n0.0\n0.0\n-0.0')" ]]

# Each operation and type, at the operation's own location.
cat > "$TMP/multiply.qui" <<'QUI'
real64 big = inf
print("before")
print(big * 0.0)
print("after")
QUI
fails multiply 101 NONFINITE_RESULT 'real64 multiplication produced NaN' '3:7'
[[ "$(cat "$TMP/multiply.out")" == "before" ]]

cat > "$TMP/add32.qui" <<'QUI'
real32 big = inf
real32 sum = big + -big
print(sum)
QUI
fails add32 101 NONFINITE_RESULT 'real32 addition produced NaN' '2:14'

cat > "$TMP/subtract.qui" <<'QUI'
real64 big = inf
real64 low = big - big
print(low)
QUI
fails subtract 101 NONFINITE_RESULT 'real64 subtraction produced NaN' '2:14'

cat > "$TMP/divide_overflow.qui" <<'QUI'
real64 big = inf
real64 small = inf
print(big / small)
QUI
fails divide_overflow 101 NONFINITE_RESULT 'real64 division produced NaN' '3:7'

cat > "$TMP/divide_zero.qui" <<'QUI'
real32 one = 0.0
real32 zero = 0.0
print(one / zero)
QUI
fails divide_zero 101 DIVISION_BY_ZERO 'real32 division by zero' '3:7'

cat > "$TMP/zero_zero.qui" <<'QUI'
real64 zero = 0.0
real64 negative_zero = -0.0
print(zero / negative_zero)
QUI
fails zero_zero 101 DIVISION_BY_ZERO 'real64 division by zero' '3:7'

cat > "$TMP/power.qui" <<'QUI'
real64 ten = 10.0
real64 exponent = 400.0
print(ten ^ exponent)
QUI
[[ "$("$QUIDRA" run "$TMP/power.qui")" == "inf" ]]

cat > "$TMP/compound.qui" <<'QUI'
real64 value = inf
value *= 0.0
print(value)
QUI
fails compound 101 NONFINITE_RESULT 'real64 multiplication produced NaN' '2:1'

# Two indeterminate operands: the earlier operation is reported at its own
# location. Valid infinite operands whose consumer makes NaN report the consumer.
cat > "$TMP/first.qui" <<'QUI'
real64 big = inf
real64 r = big * 0.0 - big * 0.0
print(r)
QUI
fails first 101 NONFINITE_RESULT 'real64 multiplication produced NaN' '2:12'

cat > "$TMP/second.qui" <<'QUI'
real64 big = inf
real64 one = 1.0
real64 r = one * 2.0 - big * 0.0
print(r)
QUI
fails second 101 NONFINITE_RESULT 'real64 multiplication produced NaN' '3:24'

cat > "$TMP/consumer.qui" <<'QUI'
real64 big = inf
real64 r = big * 1.5 - big * 1.5
print(r)
QUI
fails consumer 101 NONFINITE_RESULT 'real64 subtraction produced NaN' '2:12'

# The checks of one expression merge into one hot-path test; the loop with
# one checked addition and a fixed-width counter keeps a flag and tests it
# at its exit. A managed counter may allocate, so its check cannot be sunk.
cat > "$TMP/merged.qui" <<'QUI'
real64 sum(real64 a, real64 b, real64 c)
    return a * b + c

real64 total(real64 step)
    real64 s = -inf
    for 0 <= nat8 i < 4
        s = s + step
    return s

print(sum(1.0, 2.0, 3.0))
print(NL)
print(total(0.5))
print(NL)
print(total(inf))
print(NL)
QUI
"$QUIDRA" ir "$TMP/merged.qui" >"$TMP/merged.ir"
grep -q 'finite.check %[0-9]* (+ %[0-9]*, %[0-9]*) covers %[0-9]* (\* %[0-9]*, %[0-9]*)' "$TMP/merged.ir"
grep -q 'finite.flag \$finite.flag.[0-9]*, %[0-9]* (+ ' "$TMP/merged.ir"
grep -q 'finite.flag.check \$finite.flag.[0-9]* (+)' "$TMP/merged.ir"
fails merged 101 NONFINITE_RESULT 'real64 addition produced NaN' '7:13'
[[ "$(cat "$TMP/merged.out")" == "$(printf '5.0\n-inf')" ]]

# The machine report names the type and the operation.
QUIDRA_ERROR_FORMAT=json "$QUIDRA" run "$TMP/multiply.qui" >"$TMP/json.out" 2>"$TMP/json.err" || true
grep -q '"code": "NONFINITE_RESULT"' "$TMP/json.err"
grep -q '"details": {"type": "real64", "operation": "multiplication"}' "$TMP/json.err"
QUIDRA_ERROR_FORMAT=json "$QUIDRA" run "$TMP/divide_zero.qui" >"$TMP/json.out" 2>"$TMP/json.err" || true
grep -q '"code": "DIVISION_BY_ZERO"' "$TMP/json.err"
grep -q '"details": {"type": "real32"}' "$TMP/json.err"

# Constant overflow/division obey the same infinity policy as runtime values.
cat > "$TMP/constant_infinity.qui" <<'QUI'
real64 a = 1.0e308 * 10.0
real32 b = 3.0e38 + 3.0e38
real64 c = 1.0 / 0.0
real64 d = 1.0e308 * 2.5 / 10.0
real64 fine = 1.0e300 * 10.0
print("{a} {b} {c} {d} {fine}")
QUI
[[ "$("$QUIDRA" run "$TMP/constant_infinity.qui")" == "inf inf inf inf 1.0e+301" ]]

# A constant NaN, including one hidden by a later power to zero, is rejected.
cat > "$TMP/constant.qui" <<'QUI'
real64 a = (1.0 / 0.0) - (1.0 / 0.0)
real32 b = (1.0 / 0.0) * 0.0
real64 c = 0.0 / 0.0
real64 d = ((1.0 / 0.0) - (1.0 / 0.0)) ^ 0.0
real64 fine = 1.0e300 * 10.0
QUI
rc=0
"$QUIDRA" check "$TMP/constant.qui" >"$TMP/constant.out" 2>&1 || rc=$?
[[ "$rc" -ne 0 ]]
grep -q '1:.*error\[REAL_RANGE\] Constant real64 expression produces NaN:' "$TMP/constant.out"
grep -q '2:.*error\[REAL_RANGE\] Constant real32 expression produces NaN:' "$TMP/constant.out"
grep -q '3:.*error\[DIVIDE_BY_ZERO\] Indeterminate real division 0 / 0 is known at compile time.' "$TMP/constant.out"
grep -q '4:.*error\[REAL_RANGE\] Constant real64 expression produces NaN:' "$TMP/constant.out"
[[ "$(grep -c 'error\[' "$TMP/constant.out")" -eq 4 ]]

# Parsing accepts signed infinity but rejects NaN and finite range overflow.
cat > "$TMP/parse.qui" <<'QUI'
string[] texts = ["nan", "inf", "-inf", "1.0e999", "-1.0e999", "1.5"]
for auto text = texts[]
    real64 | error value = real64.parse(text)
    match value
        real64 parsed
            print(parsed)
        error problem
            print("error")
    print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/parse.qui")" == "$(printf 'error\ninf\n-inf\nerror\nerror\n1.5')" ]]


# Tensors: every element of a real tensor operation is tested; on the CPU the
# first failing element fails at the operation.
cat > "$TMP/tensor_overflow.qui" <<'QUI'
tensor<real32> big = tensor.ones<real32>([3]) * real32(3.0e38)
print(big[1].item())
print(NL)
tensor<real32> twice = big + big
print(twice[1].item())
QUI
[[ "$("$QUIDRA" run "$TMP/tensor_overflow.qui")" == "$(printf '3.0e+38\ninf')" ]]
cat > "$TMP/tensor_nan.qui" <<'QUI'
tensor<real32> infinity = tensor.ones<real32>([3]) * real32(inf)
print("before")
tensor<real32> invalid = infinity - infinity
QUI
fails tensor_nan 101 NONFINITE_RESULT 'tensor<real32> subtraction produced NaN' '3:26'
[[ "$(cat "$TMP/tensor_nan.out")" == "before" ]]

cat > "$TMP/tensor_divide.qui" <<'QUI'
tensor<real64> ones = tensor.ones<real64>([2, 2])
tensor<real64> zeros = ones - ones
tensor<real64> quotient = ones / zeros
print(quotient[0, 0].item())
QUI
[[ "$("$QUIDRA" run "$TMP/tensor_divide.qui")" == "inf" ]]
cat > "$TMP/tensor_zero_zero.qui" <<'QUI'
tensor<real64> zeros = tensor.zeros<real64>([2, 2])
tensor<real64> quotient = zeros / zeros
QUI
fails tensor_zero_zero 101 DIVISION_BY_ZERO 'tensor<real64> division by zero' '2:27'

cat > "$TMP/tensor_power.qui" <<'QUI'
tensor<real32> zeros = tensor.zeros<real32>([2])
tensor<real32> fine = zeros ^ real32(2.0)
print(fine[0].item())
print(NL)
tensor<real32> undefined = zeros ^ real32(0.0)
QUI
fails tensor_power 101 POWER_DOMAIN '0 ^ 0 is undefined' '5:28'

cat > "$TMP/tensor_negative_base.qui" <<'QUI'
tensor<real64> negative = tensor.ones<real64>([2]) * -2.0
tensor<real64> root = negative ^ 0.5
QUI
fails tensor_negative_base 101 POWER_DOMAIN 'a negative base requires an integer exponent' '2:23'

cat > "$TMP/tensor_integer_power.qui" <<'QUI'
tensor<int32> zeros = tensor.zeros<int32>([2])
tensor<int32> result = zeros ^ int32(0)
QUI
fails tensor_integer_power 101 POWER_DOMAIN '0 ^ 0 is undefined' '2:24'

# A gradient a backward pass delivers is tested: 1 / w with w = 1e-20 is
# finite, its gradient -1 / w^2 overflows real32.
cat > "$TMP/gradient.qui" <<'QUI'
tensor<real32> w = (tensor.ones<real32>([1]) * real32(1.0e-20)).track()
tensor<real32> z = tensor.ones<real32>([1]) / w
print("{z.untrack()[0].item() > real32(1.0e19)}")
print(NL)
z.backward(&w)
print(w.grad[0].item())
QUI
[[ "$("$QUIDRA" run "$TMP/gradient.qui")" == "$(printf 'true\n-inf')" ]]

# The results of finite tensor arithmetic are unchanged.
cat > "$TMP/tensor_finite.qui" <<'QUI'
tensor<real32> ones = tensor.ones<real32>([3])
tensor<real32> twos = ones + ones
tensor<real32> scaled = twos * twos / real32(3.0) - ones
print("{scaled[0].item()} {(twos ^ real32(3.0))[2].item()}")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/tensor_finite.qui")" == "0.33333337 8.0" ]]

# Package functions: a real tensor an extern function writes is validated
# after the call unless the package declares the function.
mkdir -p "$TMP/packages/finite_pkg/native"
cat > "$TMP/packages/finite_pkg/quidra.package" <<'MANIFEST'
name = finite_pkg
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST
cat > "$TMP/packages/finite_pkg/main.qui" <<'QUI'
extern int32 fill_native(tensor<real32> &output, real32 value) = "qtest_finite_fill"
extern int32 fill_preserving_native(tensor<real32> &output, real32 value) = "qtest_finite_fill_preserving"
extern int32 fill_checked_native(tensor<real32> &output, int32 overflow) = "qtest_finite_fill_checked"

tensor<real32> fill(nat count, real32 value)
    tensor<real32> output = tensor.zeros<real32>([count])
    int32 status = fill_native(&output, value)
    return output

tensor<real32> fill_preserving(nat count, real32 value)
    tensor<real32> output = tensor.zeros<real32>([count])
    int32 status = fill_preserving_native(&output, value)
    return output

tensor<real32> fill_checked(nat count, int32 overflow)
    tensor<real32> output = tensor.zeros<real32>([count])
    int32 status = fill_checked_native(&output, overflow)
    return output
QUI
cat > "$TMP/packages/finite_pkg/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

#include <limits>

namespace {
// Writes `value` into every element; the third element becomes NaN
// when `value` is negative, a value the package computed internally.
int fill(void* output, float value) {
    auto* data = static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!data) return 1;
    const auto count = qcore_tensor_element_count(output);
    for (unsigned long long i = 0; i < count; ++i) data[i] = value < 0.0F && i == 2
        ? std::numeric_limits<float>::quiet_NaN() : value;
    return 0;
}
} // namespace

extern "C" int qtest_finite_fill(void* output, float value) { return fill(output, value); }

extern "C" int qtest_finite_fill_preserving(void* output, float value) {
    qcore_op_register_flags("qtest_finite_fill_preserving", QCORE_OP_FINITE_PRESERVING);
    return fill(output, value < 0.0F ? -value : value);
}

extern "C" int qtest_finite_fill_checked(void* output, int overflow) {
    qcore_op_register_flags("qtest_finite_fill_checked", QCORE_OP_VALIDATES_NONFINITE);
    if (fill(output, 1.0F) != 0) return 1;
    return qcore_tensor_report_status(output, "finite_pkg.fill_checked",
                                      overflow != 0 ? 1u : 0u);
}
CPP
cat > "$TMP/package_ok.qui" <<'QUI'
import finite_pkg
tensor<real32> a = finite_pkg.fill(3, real32(1.5))
tensor<real32> b = finite_pkg.fill_preserving(3, real32(-2.5))
tensor<real32> c = finite_pkg.fill_checked(3, 0)
print("{a[2].item()} {b[2].item()} {c[2].item()}")
print(NL)
QUI
[[ "$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/package_ok.qui")" == "1.5 2.5 1.0" ]]
cat > "$TMP/package_infinity.qui" <<'QUI'
import finite_pkg
tensor<real32> a = finite_pkg.fill(3, real32(inf))
print(a[2].item())
QUI
[[ "$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/package_infinity.qui")" == "inf" ]]
cat > "$TMP/package_fallback.qui" <<'QUI'
import finite_pkg
print("before")
tensor<real32> a = finite_pkg.fill(3, real32(-1.0))
print("after")
QUI
rc=0
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/package_fallback.qui" \
    >"$TMP/package_fallback.out" 2>"$TMP/package_fallback.err" || rc=$?
[[ "$rc" -eq 101 ]]
[[ "$(cat "$TMP/package_fallback.out")" == "before" ]]
grep -q 'Quidra runtime error\[NONFINITE_RESULT\] at .*package_fallback.qui:3:1$' "$TMP/package_fallback.err"
grep -q '| package operation .*fill_native produced a NaN real32 value$' "$TMP/package_fallback.err"
cat > "$TMP/package_checked.qui" <<'QUI'
import finite_pkg
tensor<real32> a = finite_pkg.fill_checked(3, 1)
QUI
rc=0
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" run "$TMP/package_checked.qui" \
    >"$TMP/package_checked.out" 2>"$TMP/package_checked.err" || rc=$?
[[ "$rc" -eq 101 ]]
grep -q 'Quidra runtime error\[NONFINITE_RESULT\] at .*package_checked.qui:2:1$' "$TMP/package_checked.err"
grep -q '| package operation finite_pkg.fill_checked produced a NaN real32 value$' "$TMP/package_checked.err"

# Metal: the failure of a real kernel is deferred to the next synchronization
# point and names the operation and the statement that queued it.
if "$QUIDRA" gpu 2>/dev/null | grep -Fq "backend: Metal"; then
    cat > "$TMP/metal_overflow.qui" <<'QUI'
tensor<real32> big = tensor.ones<real32>([4], gpu = 0) * real32(inf)
tensor<real32> twice = big - big
print("queued")
print(NL)
print(twice.cpu()[0].item())
QUI
    rc=0
    "$QUIDRA" run "$TMP/metal_overflow.qui" >"$TMP/metal_overflow.out" 2>"$TMP/metal_overflow.err" || rc=$?
    [[ "$rc" -eq 101 ]]
    [[ "$(cat "$TMP/metal_overflow.out")" == "queued" ]]
    grep -Fq "tensor<real32> subtraction produced NaN (deferred GPU check from $TMP/metal_overflow.qui:2:" "$TMP/metal_overflow.err"
    cat > "$TMP/metal_finite.qui" <<'QUI'
tensor<real32> ones = tensor.ones<real32>([4], gpu = 0)
tensor<real32> twos = ones + ones
print((twos * twos / real32(3.0)).cpu()[3].item())
print(NL)
QUI
    [[ "$("$QUIDRA" run "$TMP/metal_finite.qui")" == "1.3333334" ]]
else
    echo "nonfinite_tests: Metal checks skipped (no Metal device)"
fi
