#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$(realpath "$1")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/bindings.qui" <<'QUI'
real32 cross_integer := 5
int cross_real := 3.0
com16 cross_complex := 5
int64 source := 7
int8 narrow := source
real16 rounded := 2049
real64 integral := 4.0
narrow = integral
int32 from_real(real64 value)
    return value
real16 from_integer(int32 value)
    return value
int8 default_byte(int8 value := int64(7))
    return value
class Defaults(int8 value := real64(6.0))
    real16 rounded := int32(2049)
    int8 member_default(int8 input := int64(5))
        return input
int64[] original := [7, 8]
int8[] bytes := original
real64[] fractions := [3.0, 4.0]
bytes = fractions
int8[] array_return(real64[] values)
    return values
tensor<int64>{2} original_tensor := tensor.ones<int64>([2]) * 7
tensor<int8>{2} byte_tensor := original_tensor
tensor<real64>{2} integral_tensor := tensor.ones<real64>([2]) * 4.0
byte_tensor = integral_tensor
tensor<int8> tensor_return(tensor<real64> values)
    return values
class NumericField(int8 amount)
field := NumericField(1)
field.amount = source
int8[1] indexed := [1]
indexed[0] = source
bytes[0:<2] = original
int8 | error union_return(real64 value)
    return value
int8 | none union_value := integral
int8 | error source_error()
    return error("original", code = "ORIGINAL_ERROR")
int8 | error preserved_error := source_error()
println(cross_integer == 5.0 and cross_real == 3 and cross_complex.real == 5.0)
println(narrow == 4 and rounded == 2048.0)
println(from_real(integral) == 4 and from_integer(2049) == 2048.0)
println(default_byte() == 7 and Defaults().value == 6)
println(Defaults().rounded == 2048.0 and Defaults().member_default() == 5)
println(bytes == [7, 8] and original == [7, 8])
println(array_return(fractions) == [3, 4])
println(byte_tensor[0].item() == 4 and original_tensor[0].item() == 7)
println(tensor_return(integral_tensor)[0].item() == 4)
println(field.amount == 7 and indexed[0] == 7)
match union_return(integral)
    int8 value
        println(value == 4)
    error problem
        println(false)
match union_value
    int8 value
        println(value == 4)
    none
        println(false)
match preserved_error
    int8 value
        println(false)
    error problem
        println(problem.code == "ORIGINAL_ERROR")
QUI
"$QUIDRA" run "$TMP/bindings.qui" > "$TMP/actual"
diff -u <(printf 'true\n%.0s' {1..13}) "$TMP/actual"

# Literal/generator destinations convert each value after its own numeric
# materialization; heterogeneous numeric source representations are valid.
cat > "$TMP/contextual_arrays.qui" <<'QUI'
real32[] values := [1, 2.5]
int8[] integers := [1.0, 2]
int64 source := 7
real16[][] nested := [[source, 2049], [1.0, 2]]
real16[] generated := [for item := [source][] then if item > 0 then item else 1.5]
real32[] take(real32[] input)
    return input
println(values == [1.0, 2.5] and integers == [1, 2])
println(nested[0][1] == 2048.0 and generated[0] == 7.0)
println(take([source, 2.5]) == [7.0, 2.5])
nat[] ancestors := []
ancestor := 0
ancestors = [nat(ancestor)].concat(ancestors)
nat[] make_ancestors(int input)
    return [nat(input)].concat([])
nat[] keep_ancestors(nat[] input)
    return input
println(ancestors == [0])
println(make_ancestors(1) == [1])
println(keep_ancestors([nat(ancestor)].concat([])) == [0])
QUI
"$QUIDRA" run "$TMP/contextual_arrays.qui" > "$TMP/contextual_arrays.actual"
diff -u <(printf 'true\n%.0s' {1..6}) "$TMP/contextual_arrays.actual"

cat > "$TMP/compound.qui" <<'QUI'
real32 delta := 0.0
int32 scalar := 16777217
scalar += delta
println(scalar == 16777216)
int32[] values := [16777217]
values += delta
println(values[0] == 16777216)
real16 rounded := 2048.0
real32 increment := 1.0
rounded += increment
println(rounded == 2048.0)
class Box(int32 value)
box := Box(16777217)
box.value += delta
println(box.value == 16777216)
int32 original := 16777217
int32 &view := &original
view += delta
println(original == 16777216)
int choose(int &calls)
    calls += 1
    return 0
real32 replace(int32[] &items)
    items = [10, 20]
    return 0.0
calls := 0
int32[] items := [16777217]
items[choose(&calls)] += replace(&items)
println(calls == 1 and items[0] == 16777216 and items[1] == 20)
class Holder(int32 &value)
    real32 .switch(int32 &next)
        &value = &next
        return 0.0
    void .update(int32 &next)
        value += .switch(&next)
int32 old_value := 16777217
int32 next := 10
holder := Holder(&old_value)
.holder.update(&next)
println(old_value == 16777217 and next == 16777216)
QUI
"$QUIDRA" run "$TMP/compound.qui" > "$TMP/compound.actual"
diff -u <(printf 'true\n%.0s' {1..7}) "$TMP/compound.actual"

fails() {
    local name="$1" status=0
    cat > "$TMP/$name.qui"
    "$QUIDRA" run "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err" || status=$?
    if [[ "$status" -ne 101 ]] || ! grep -q 'Quidra runtime error\[NUMERIC_CONVERSION\]' "$TMP/$name.err"; then
        cat "$TMP/$name.err" >&2
        printf 'case %s: expected runtime NUMERIC_CONVERSION (101), got %s\n' "$name" "$status" >&2
        return 1
    fi
    [[ ! -s "$TMP/$name.out" ]]
}
fails declaration_range <<'QUI'
int64 source := 128
int8 value := source
println("after")
QUI
fails assignment_fractional <<'QUI'
real64 source := 3.5
int32 value := 0
value = source
println("after")
QUI
fails return_fractional <<'QUI'
int32 convert(real64 source)
    return source
println(convert(3.5))
QUI
fails default_range <<'QUI'
int8 convert(int8 source := int64(128))
    println("entered")
    return source
println(convert())
QUI
fails array_range <<'QUI'
int64[] source := [1, 128]
int8[] value := source
println("after")
QUI
fails array_literal_fractional <<'QUI'
int64 source := 1
int8[] values := [source, 2.5]
println("after")
QUI
fails tensor_fractional <<'QUI'
tensor<real64> source := tensor.ones<real64>([2]) * 2.5
tensor<int8> value := source
println("after")
QUI
fails literal_fractional <<'QUI'
int value := 3.5
println("after")
QUI
fails literal_negative_natural <<'QUI'
nat value := -1
println("after")
QUI
fails compound_fractional <<'QUI'
int32 value := 1
real32 increment := 0.5
value += increment
println("after")
QUI
fails compound_range <<'QUI'
int8 value := 127
int64 increment := 1
value += increment
println("after")
QUI
fails compound_array_range <<'QUI'
int8[] values := [1, 127]
int64 increment := 1
values += increment
println("after")
QUI

rejects() {
    local name="$1"
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err"; then
        printf 'case %s unexpectedly accepted\n' "$name" >&2
        return 1
    fi
    grep -q 'TYPE_MISMATCH\|SPECIALIZATION_NO_MATCH\|UNKNOWN_MEMBER' "$TMP/$name.err"
}
for declaration in 'bool source := true' 'string source := "1"' 'bin source := bin("01")' 'com32 source := 1.0 + 0.0i'; do
    for destination in int32 real32; do
        rejects nonnumeric_source <<QUI
$declaration
$destination result := source
QUI
    done
done
for destination in bool string bin; do
    rejects nonnumeric_destination <<QUI
int64 source := 1
$destination result := source
QUI
done
rejects reference_storage <<'QUI'
void change(int8 &value)
    value = 1
int64 source := 3
change(&source)
QUI
rejects array_boolean <<'QUI'
int64[] source := [0, 1]
bool[] result := source
QUI
rejects tensor_boolean <<'QUI'
tensor<int64> source := tensor.ones<int64>([2])
tensor<bool> result := source
QUI
