#!/usr/bin/env bash
# Numeric operators convert to a statically selected common type before work.
set -euo pipefail
QUIDRA="$(realpath "$1")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/common.qui" <<'QUI'
a := 5 + 6
int32 contextual := 5 + 6
nearby := contextual + 1
numeric_type_value_0 := a
int numeric_type_proof_0 := numeric_type_value_0
print((a == 11))
print(NL)
numeric_type_value_1 := nearby
int32 numeric_type_proof_1 := numeric_type_value_1
print((nearby == 12))
print(NL)
nat64 natural := 7
int32 integer := -2
mixed := natural + integer
numeric_type_value_2 := mixed
int32 numeric_type_proof_2 := numeric_type_value_2
print((mixed == 5))
print(NL)
numeric_type_value_3 := natural
nat64 numeric_type_proof_3 := numeric_type_value_3
print((natural == 7))
print(NL)
nat arbitrary_natural := 9
numeric_type_value_4 := arbitrary_natural + integer
int32 numeric_type_proof_4 := numeric_type_value_4
print((arbitrary_natural + integer == 7))
print(NL)
int arbitrary_integer := -3
numeric_type_value_5 := arbitrary_integer + natural
int numeric_type_proof_5 := numeric_type_value_5
print((arbitrary_integer + natural == 4))
print(NL)
int32 halfway := 2049
real16 half_zero := 0.0
rounded := halfway + half_zero
numeric_type_value_6 := rounded
real16 numeric_type_proof_6 := numeric_type_value_6
print((rounded == 2048.0))
print(NL)
numeric_type_value_7 := halfway
int32 numeric_type_proof_7 := numeric_type_value_7
print((halfway == 2049))
print(NL)
int64 single_halfway := 16777217
real32 single_zero := 0.0
single_rounded := single_halfway + single_zero
numeric_type_value_8 := single_rounded
real32 numeric_type_proof_8 := numeric_type_value_8
print((single_rounded == 16777216.0))
print(NL)
// Just above a bfloat16 midpoint: rounding through real64 would lose +1.
int exact_above_midpoint := 37037603335494959105
real16b bfloat_zero := 0.0
bfloat_once := exact_above_midpoint + bfloat_zero
print(real(bfloat_once) == 37181718523570814976.0)
print(NL)
// Integer conversions round once even just above an IEEE midpoint.
int tagged_midpoint := 1152921573326323713
int boxed_single_midpoint := 1208925891672223212634113
int boxed_double_midpoint := 1208925819614629308923905
real64 double_zero := 0.0
print(real(tagged_midpoint + single_zero) == 1152921642045800448.0)
print(NL)
print(real(boxed_single_midpoint + single_zero) == 1208925963729817250562048.0)
print(NL)
print(real(boxed_double_midpoint + double_zero) == 1208925819614629443141632.0)
print(NL)
real32 explicit_single := real32(boxed_single_midpoint)
real64 explicit_double := real64(boxed_double_midpoint)
print(real(explicit_single) == 1208925963729817250562048.0 and real(explicit_double) == 1208925819614629443141632.0)
print(NL)
real16 half := 1.5
real16b bhalf := 2.0
numeric_type_value_9 := half + bhalf
real32 numeric_type_proof_9 := numeric_type_value_9
print((half + bhalf == 3.5))
print(NL)
com16 z := 0.0 + 1.0i
real64 fine := 1.0006
complex_rounded := z + fine
original_fine := fine
real64 original_fine_type_proof := original_fine
numeric_type_value_10 := complex_rounded
com16 numeric_type_proof_10 := numeric_type_value_10
print((complex_rounded.real == 1.0009765625 and complex_rounded.imag == 1.0))
print(NL)
com32 wide_z := 2.0 + 3.0i
complex_wide := z + wide_z
numeric_type_value_11 := complex_wide
com32 numeric_type_proof_11 := numeric_type_value_11
print((complex_wide == 2.0 + 4.0i))
print(NL)
com16b bz := 2.0 + 3.0i
numeric_type_value_12 := z + bz
com32 numeric_type_proof_12 := numeric_type_value_12
print((z + bz == 2.0 + 4.0i))
print(NL)
complex_exact := arbitrary_integer + wide_z
numeric_type_value_13 := complex_exact
com32 numeric_type_proof_13 := numeric_type_value_13
print((complex_exact == -1.0 + 3.0i))
print(NL)
print((integer < natural and natural > integer and natural != integer))
print(NL)
int32 lower := 0
nat64 middle := 2
int64 upper := 3
numeric_type_value_14 := middle
nat64 numeric_type_proof_14 := numeric_type_value_14
print((lower < middle < upper))
print(NL)
int8 narrow := 1
nat64 outside := 256
print((not (upper < middle < narrow < outside)))
print(NL)
nat64[2] naturals := [7, 8]
int32[2] integers := [-2, -3]
array_sum := naturals + integers
original_naturals := naturals
nat64[2] original_naturals_type_proof := original_naturals
numeric_type_value_15 := array_sum[0]
int32 numeric_type_proof_15 := numeric_type_value_15
print((array_sum[0] == 5 and array_sum[1] == 5 and naturals[0] == 7))
print(NL)
array_scalar := halfway + [half_zero, half_zero]
numeric_type_value_16 := array_scalar[0]
real16 numeric_type_proof_16 := numeric_type_value_16
print((array_scalar[0] == 2048.0))
print(NL)
array_compare := naturals > integers
print((array_compare[0] and array_compare[1]))
print(NL)
nat64[][] nested_naturals := [[7, 8]]
int32[][] nested_integers := [[-2, -3]]
nested_sum := nested_naturals + nested_integers
numeric_type_value_17 := nested_sum[0][0]
int32 numeric_type_proof_17 := numeric_type_value_17
print((nested_sum[0][1] == 5))
print(NL)
tensor<nat64> natural_tensor := tensor.ones<nat64>([2])
tensor<int32> integer_tensor := tensor.ones<int32>([2])
tensor_sum := natural_tensor + integer_tensor
original_natural_tensor := natural_tensor
tensor<nat64> original_natural_tensor_type_proof := original_natural_tensor
numeric_type_value_18 := tensor_sum[0].item()
int32 numeric_type_proof_18 := numeric_type_value_18
print((tensor_sum[0].item() == 2 and natural_tensor[0].item() == 1))
print(NL)
tensor_compare := natural_tensor == integer_tensor
print(tensor_compare.all())
print(NL)
tensor<real16> half_tensor := tensor.zeros<real16>([2])
tensor_rounded := halfway + half_tensor
numeric_type_value_19 := tensor_rounded[0].item()
real16 numeric_type_proof_19 := numeric_type_value_19
print((tensor_rounded[0].item() == 2048.0))
print(NL)
tensor<int32> halfway_tensor := tensor.ones<int32>([2]) * halfway
tensor_mixed := halfway_tensor + half_tensor
numeric_type_value_20 := tensor_mixed[0].item()
real16 numeric_type_proof_20 := numeric_type_value_20
print((tensor_mixed[0].item() == 2048.0))
print(NL)
QUI
# Each success assertion prints exactly one boolean line.
python3 - "$TMP/common.qui" "$TMP/expected" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text('true\n' * sum(line.startswith('print(') and line != 'print(NL)' for line in source.splitlines()))
PY
"$QUIDRA" run "$TMP/common.qui" > "$TMP/actual"
diff -u "$TMP/expected" "$TMP/actual"

fails() {
    local name="$1" code="$2" status=0
    cat > "$TMP/$name.qui"
    "$QUIDRA" run "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err" || status=$?
    if [[ "$status" -ne 101 ]] || ! grep -q "Quidra runtime error\[$code\]" "$TMP/$name.err"; then
        cat "$TMP/$name.err" >&2
        printf 'case %s: expected runtime %s (101), got status %s\n' "$name" "$code" "$status" >&2
        return 1
    fi
}
# The mathematical result fits, but the left operand cannot first become int32.
fails unsigned_before_cancellation NUMERIC_CONVERSION <<'QUI'
nat64 large := 2147483648
int32 negative := -2147483648
print(large + negative)
QUI
fails arbitrary_before_cancellation NUMERIC_CONVERSION <<'QUI'
nat large := 2147483648
int32 negative := -2147483648
print(large + negative)
QUI
fails half_before_multiply NUMERIC_CONVERSION <<'QUI'
int32 large := 65520
real16 zero := 0.0
print(large * zero)
QUI
fails complex_before_multiply NUMERIC_CONVERSION <<'QUI'
real64 large := 65520.0
com16 zero := 0.0 + 0.0i
print(large * zero)
QUI
fails comparison_conversion NUMERIC_CONVERSION <<'QUI'
nat64 large := 2147483648
int32 small := 0
print(large > small)
QUI
fails chain_conversion NUMERIC_CONVERSION <<'QUI'
int64 lower := -1
nat64 large := 2147483648
int32 small := 0
print(lower < large < small)
QUI
fails result_overflow INTEGER_OVERFLOW <<'QUI'
nat64 small := 1
int8 maximum := 127
print(small + maximum)
QUI
fails array_conversion NUMERIC_CONVERSION <<'QUI'
nat64[1] large := [2147483648]
int32[1] negative := [-2147483648]
print((large + negative)[0])
QUI
fails tensor_conversion NUMERIC_CONVERSION <<'QUI'
tensor<nat64> large := tensor.ones<nat64>([1]) * 2147483648
tensor<int32> negative := tensor.ones<int32>([1]) * -2147483648
print((large + negative)[0].item())
QUI
