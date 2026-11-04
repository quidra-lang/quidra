#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$(realpath "$1")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/calls.qui" <<'QUI'
int8 take_byte(int8 value)
    return value
int32 take_integer(int32 value)
    return value
real16 take_half(real16 value)
    return value
com16 take_complex(com16 value)
    return value
int8[] take_array(int8[] values)
    return values
tensor<int8> take_tensor(tensor<int8> values)
    return values
class NumericBox this(local int8 initial)
    int8 value
    this.value = initial
    int8 narrowed(int8 input)
        return input
int64 wider := 7
real64 integral := 3.0
int32 halfway := 2049
real64 precision := 1.0006
print(take_byte(wider) == 7)
print(NL)
print(take_integer(integral) == 3 and take_integer(3.0) == 3)
print(NL)
print(take_half(halfway) == 2048.0)
print(NL)
print(take_complex(precision).real == 1.0009765625 and take_complex(precision).imag == 0.0)
print(NL)
int64[2] original := [7, 8]
converted := take_array(original)
numeric_type_value_0 := original[0]
int64 numeric_type_proof_0 := numeric_type_value_0
print(converted[0] == 7 and converted[1] == 8)
print(NL)
tensor<int64> original_tensor := tensor.ones<int64>([2]) * 7
converted_tensor := take_tensor(original_tensor)
numeric_type_value_1 := original_tensor[0].item()
int64 numeric_type_proof_1 := numeric_type_value_1
print(converted_tensor[0].item() == 7)
print(NL)
NumericBox box := NumericBox(wider)
print(box.value == 7 and box.narrowed(wider) == 7)
print(NL)
func<int8>(int8) callback := take_byte
print(callback(wider) == 7)
print(NL)
int32 accumulator := -2
nat64 addend := 7
accumulator += addend
numeric_type_value_2 := addend
nat64 numeric_type_proof_2 := numeric_type_value_2
print(accumulator == 5 and addend == 7)
print(NL)
real16 half_accumulator := 0.0
half_accumulator += halfway
print(half_accumulator == 2048.0)
print(NL)
int32[1] indexed := [-2]
indexed[0] += addend
print(indexed[0] == 5)
print(NL)
int integral_literal := int(3.0)
print(integral_literal == 3)
print(NL)
real64 count := 3.0
print(string.repeat("x", count) == "xxx")
print(NL)
int8[] filled := array(count, fill = wider)
print(len(filled) == 3 and filled[0] == 7)
print(NL)
int64[] integer_shape := [2]
tensor<int32> integer_shaped := tensor.ones<int32>(integer_shape)
print(integer_shaped[1].item() == 1)
print(NL)
real64[] shape := [2.0]
tensor<real16> shaped := tensor.ones<real16>(shape)
print(shaped[1].item() == 1.0)
print(NL)
map<int32, real32> lookup
int64 key := 7
int16 item := 3
.lookup.set(key, item)
print(lookup.has(key) and lookup.has(int32(key)))
print(NL)
match lookup.get(key)
    real32 value
        print(value == real32(item))
        print(NL)
    none
        print(false)
        print(NL)
result := lookup.get(key)
match result
    real32 value
        print(value == real32(item))
        print(NL)
    none
        print(false)
        print(NL)
real64 rounded_item := 0.1
real32 expected_item := real32(rounded_item)
.lookup.set(key, rounded_item)
match lookup.get(key)
    real32 value
        print(value == expected_item)
        print(NL)
    none
        print(false)
        print(NL)
distinct<int32> unique
.unique.add(key)
print(unique.has(key) and unique.has(int32(key)))
print(NL)
int64 original_key := key
int16 original_item := item
print(original_key == 7 and original_item == 3)
print(NL)
QUI
python3 - "$TMP/calls.qui" "$TMP/expected" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text('true\n' * sum(line.lstrip().startswith('print(') and line.strip() not in ('print(NL)', 'print(false)') for line in source.splitlines()))
PY
"$QUIDRA" run "$TMP/calls.qui" > "$TMP/actual"
diff -u "$TMP/expected" "$TMP/actual"
# Generic inference sees the same success/container type as the checker.
cat > "$TMP/generic_cast_containers.qui" <<'QUI'
tensor<T> identity<T: numeric>(tensor<T> value)
    return value
T[] array_identity<T: numeric>(T[] value)
    return value
tensor<int64> | error make_tensor()
    return tensor.ones<int64>([2])
tensor<nat8>{2} source := tensor.ones<nat8>([2])
converted := int64(source)
result := identity(converted)
direct := identity(int64(source))
from_function := make_tensor()
function_result := identity(from_function)
nat8[] bytes := [1, 2]
converted_array := int64(bytes)
array_result := array_identity(converted_array)
direct_array := array_identity(int64(bytes))
println(result[0].item() == 1 and direct[1].item() == 1 and function_result[0].item() == 1)
println(array_result == [int64(1), 2] and direct_array == array_result)
QUI
[[ "$("$QUIDRA" run "$TMP/generic_cast_containers.qui")" == "$(printf 'true\ntrue')" ]]
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
fails generic_cast_narrowing NUMERIC_CONVERSION <<'QUI'
tensor<T> identity<T: numeric>(tensor<T> value)
    print("entered")
    return value
tensor<int64> source := tensor.ones<int64>([1]) * 128
converted := int8(source)
result := identity(converted)
print(result[0].item())
QUI
[[ ! -s "$TMP/generic_cast_narrowing.out" ]]
fails narrow NUMERIC_CONVERSION <<'QUI'
int8 take(int8 value)
    print("entered")
    return value
int64 value := 128
print(take(value))
QUI
[[ ! -s "$TMP/narrow.out" ]]
fails fractional NUMERIC_CONVERSION <<'QUI'
int32 take(int32 value)
    return value
real64 value := 3.5
print(take(value))
QUI
grep -q 'NOT_INTEGRAL\|not an integer' "$TMP/fractional.err"
fails half_overflow NUMERIC_CONVERSION <<'QUI'
real16 take(real16 value)
    return value
int32 value := 65520
print(take(value))
QUI
fails array_narrow NUMERIC_CONVERSION <<'QUI'
int8[] take(int8[] values)
    return values
int64[] values := [7, 128]
print(take(values)[0])
QUI
fails array_fractional NUMERIC_CONVERSION <<'QUI'
int8[] take(int8[] values)
    return values
real64[] values := [7.0, 2.5]
print(take(values)[0])
QUI
fails tensor_fractional NUMERIC_CONVERSION <<'QUI'
tensor<int8> take(tensor<int8> values)
    return values
tensor<real64> values := tensor.ones<real64>([2]) * 2.5
print(take(values)[0].item())
QUI
fails tensor_negative_shape NUMERIC_CONVERSION <<'QUI'
int64 n := -1
tensor<int32> values := tensor.zeros<int32>([n])
print(values[0].item())
QUI
fails tensor_unsigned_shape NUMERIC_CONVERSION <<'QUI'
nat64 n := 18446744073709551615
tensor<int32> values := tensor.zeros<int32>([n])
print(values[0].item())
QUI
fails compound_conversion NUMERIC_CONVERSION <<'QUI'
int32 accumulator := -2147483648
nat64 addend := 2147483648
accumulator += addend
print(accumulator)
QUI
fails builtin_fractional NUMERIC_CONVERSION <<'QUI'
real64 count := 3.5
print(string.repeat("x", count))
QUI
for query in map_has map_get_match map_get_value distinct_has explicit_map_get; do
    case "$query" in
        map_has) body='print(lookup.has(key))' ;;
        map_get_match) body=$'match lookup.get(key)\n    real32 value\n        print(value)\n    none\n        print(false)' ;;
        map_get_value) body=$'result := lookup.get(key)\nprint(result)' ;;
        distinct_has) body='print(unique.has(key))' ;;
        explicit_map_get) body=$'int32 converted := int32(key)\nmatch lookup.get(converted)\n    real32 value\n        print(value)\n    none\n        print(false)' ;;
    esac
    fails "$query" NUMERIC_CONVERSION <<QUI
map<int32, real32> lookup
distinct<int32> unique
int64 key := 2147483648
$body
QUI
    [[ ! -s "$TMP/$query.out" ]]
done
# A writable parameter retains its declared storage type.
cat > "$TMP/reference.qui" <<'QUI'
void update(int8 &value)
    value = 1
int64 wider := 7
update(&wider)
QUI
if "$QUIDRA" check "$TMP/reference.qui" > "$TMP/reference.out" 2> "$TMP/reference.err"; then
    echo 'numeric conversion unexpectedly changed writable-reference storage' >&2
    exit 1
fi
grep -q 'TYPE_MISMATCH\|ARGUMENT_TYPE' "$TMP/reference.err"

cat > "$TMP/cli_defaults.qui" <<'QUI'
cli args
    real64 value = argument(default = real32(0.1))
    real64 ratio = option(default = real32(0.1))
print(args.value == 0.10000000149011612)
print(NL)
print(args.ratio == 0.10000000149011612)
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/cli_defaults.qui")" == "$(printf 'true\ntrue')" ]]
[[ "$("$QUIDRA" run "$TMP/cli_defaults.qui" -- 0.25 --ratio 0.5)" == "$(printf 'false\nfalse')" ]]
fails cli_default_narrowing NUMERIC_CONVERSION <<'QUI'
cli args
    int8 value = option(default = int64(128))
print(args.value)
QUI
# Even a supplied option evaluates and converts its default before lookup.
status=0
"$QUIDRA" run "$TMP/cli_default_narrowing.qui" -- --value 1 > "$TMP/cli_present.out" 2> "$TMP/cli_present.err" || status=$?
[[ "$status" -eq 101 ]]
grep -q 'Quidra runtime error\[NUMERIC_CONVERSION\]' "$TMP/cli_present.err"
fails cli_argument_narrowing NUMERIC_CONVERSION <<'QUI'
cli args
    int8 value = argument(default = int64(128))
print(args.value)
QUI
