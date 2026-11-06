#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/array_text.qui" <<'QUI'
string int_array_text(int[] items)
    string text := "["
    bool first := true
    for int item := items[]
        if not first
            text += ", "
        text += string(item)
        first = false
    return text + "]"
string string_array_text(string[] items)
    string text := "["
    bool first := true
    for string item := items[]
        if not first
            text += ", "
        text += DQ + item + DQ
        first = false
    return text + "]"
string nested_int_array_text(int[][] items)
    string text := "["
    bool first := true
    for int[] item := items[]
        if not first
            text += ", "
        text += int_array_text(item)
        first = false
    return text + "]"
QUI
cat "$TMP/array_text.qui" > "$TMP/selection.qui"
cat >> "$TMP/selection.qui" <<'QUI'
int[] values := [-2, 0, 3, 4]
squares := [for int x := values[] if x > 0 then x * x]
print("{int_array_text(squares)}{NL}")
unchanged := [
    for item := values[]
    then item
]
print("{int_array_text(unchanged)}{NL}")
int[] empty := []
empty_result := [for int value := empty[] then value * value]
print("{int_array_text(empty_result)}{NL}")
string[] empty_strings := [for value := empty[] then "unused"]
print("{string_array_text(empty_strings)}{NL}")
int[] small := [1, 2]
nested := [for int left := small[] then [for int right := small[] then left * right]]
print("{nested_int_array_text(nested)}{NL}")
QUI
cat > "$TMP/selection.expected" <<'OUT'
[9, 16]
[-2, 0, 3, 4]
[]
[]
[[1, 2], [2, 4]]
OUT
cat "$TMP/array_text.qui" > "$TMP/evaluation.qui"
cat >> "$TMP/evaluation.qui" <<'QUI'
int[] supply(int[] values, int[] &counts)
    counts[0] = counts[0] + 1
    return values
bool select(int x, int[] &counts)
    counts[1] = counts[1] + 1
    return x > 0
int transform(int &x, int[] &counts, int[] &original)
    counts[2] = counts[2] + 1
    original = [100]
    x = x + 10
    return x * x
int[] values := [-2, 0, 3, 4]
int[] counts := [0, 0, 0]
result := [for int x := supply(values, &counts)[] if select(x, &counts) then transform(&x, &counts, &values)]
print("{int_array_text(result)}{NL}{int_array_text(counts)}{NL}{int_array_text(values)}{NL}")
int[] original := [1, 2]
int[] ignored := [0, 0, 0]
copies := [for int item := original[] then transform(&item, &ignored, &values)]
print("{int_array_text(copies)}{NL}{int_array_text(original)}{NL}")
QUI
cat > "$TMP/evaluation.expected" <<'OUT'
[169, 196]
[1, 4, 2]
[100]
[121, 144]
[1, 2]
OUT
cat "$TMP/array_text.qui" > "$TMP/propagation.qui"
cat >> "$TMP/propagation.qui" <<'QUI'
string | error produce(string value, int[] &calls)
    calls[0] = calls[0] + 1
    if value == "stop"
        return error("generation stopped", code = "GENERATION_TEST")
    return value + "!"
string[] | error generate(string[] values, int[] &calls)
    return [for string text := values[] then try produce(text, &calls)]
bool | error retain(string text, int[] &calls)
    calls[0] = calls[0] + 1
    if text == "stop"
        return error("condition stopped", code = "GENERATION_CONDITION")
    return true
string[] | error filter(string[] values, int[] &calls)
    return [for string text := values[] if try retain(text, &calls) then text]
string[] | error unavailable(int[] &calls)
    calls[0] = calls[0] + 1
    return error("source stopped", code = "GENERATION_SOURCE")
string[] | error source_failure(int[] &calls)
    return [for string text := (try unavailable(&calls))[] then text]
int[] calls := [0]
string[] | error result := generate(["first", "stop", "last"], &calls)
match result
    string[] value
        print("unexpected success" + NL)
    error problem
        print(problem.code + NL)
print("{int_array_text(calls)}{NL}")
string[] | error recovered := generate(["next"], &calls)
match recovered
    string[] value
        print("{value[0]}{NL}")
    error problem
        print("unexpected failure" + NL)
print("{int_array_text(calls)}{NL}")
string[] | error condition_result := filter(["first", "stop", "last"], &calls)
match condition_result
    string[] value
        print("unexpected condition success" + NL)
    error problem
        print(problem.code + NL)
print("{int_array_text(calls)}{NL}")
string[] | error source_result := source_failure(&calls)
match source_result
    string[] value
        print("unexpected source success" + NL)
    error problem
        print(problem.code + NL)
print("{int_array_text(calls)}{NL}")
QUI
cat > "$TMP/propagation.expected" <<'OUT'
GENERATION_TEST
[2]
next!
[3]
GENERATION_CONDITION
[5]
GENERATION_SOURCE
[6]
OUT
cat > "$TMP/primitive-items.qui" <<'QUI'
real64[] doubles := [for entry := [real64(1), real64(2)][] then entry * 2.0]
print(doubles[0] == 2.0 and doubles[1] == 4.0)
bool[] flipped := [for flag := [true, false][] then not flag]
print(not flipped[0] and flipped[1])
int8[] narrow := [for entry := [int8(3)][] then entry + 1]
print(narrow[0] == 4)
com32[] complex_values := [1.0 + 1.0i]
com32[] complex_copy := [for entry := complex_values[] then entry]
print(complex_copy == complex_values)
real64[] empty := []
empty_copy := [for entry := empty[] then entry]
print(empty_copy.len())
class Observation(real64[] values)
Observation value := Observation([for entry := [real64(1)][] then entry])
print(value.values[0] == 1.0)
print(NL)
QUI
cat > "$TMP/primitive-items.expected" <<'OUT'
truetruetruetrue0true
OUT
cat > "$TMP/inferred-cast-products.qui" <<'QUI'
real64[] weighted_values(int[] counts, real64[] scale)
    return [for count := counts[] then real64(count)] * scale
int[] counts := [1, 2]
real64[] scale := [2.0, 3.0]
weighted := [for count := counts[] then real64(count)] * scale
real64[] expected := [2.0, 6.0]
println(weighted == expected)
println(weighted_values(counts, scale) == expected)
(real64 | error)[] preserved := [for text := ["bad"][] then real64(text)]
match preserved[0]
    real64 value
        println("unexpected")
    error problem
        println("handled")
QUI
cat > "$TMP/inferred-cast-products.expected" <<'OUT'
true
true
handled
OUT
for name in selection evaluation propagation primitive-items inferred-cast-products; do
    "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name"
    "$TMP/$name" > "$TMP/$name.actual"
    cmp "$TMP/$name.expected" "$TMP/$name.actual"
done
cat > "$TMP/uninitialized.qui" <<'QUI'
int[] values := array(1)
result := [for int x := values[] if false then x]
print("unexpected success" + NL)
QUI
"$QUIDRA" build "$TMP/uninitialized.qui" -o "$TMP/uninitialized"
set +e
"$TMP/uninitialized" > "$TMP/uninitialized.out" 2> "$TMP/uninitialized.err"
status=$?
set -e
[[ "$status" == 101 && ! -s "$TMP/uninitialized.out" ]]
grep -Fq 'UNINITIALIZED' "$TMP/uninitialized.err"

cat > "$TMP/inferred-value-error-order.qui" <<'QUI'
int8 | error convert(string text)
    println(text)
    return int8(text)
result := [for text := ["1", "bad", "2"][] then convert(text)]
println("unexpected")
QUI
"$QUIDRA" build "$TMP/inferred-value-error-order.qui" -o "$TMP/inferred-value-error-order"
set +e
"$TMP/inferred-value-error-order" > "$TMP/inferred-value-error-order.out" 2> "$TMP/inferred-value-error-order.err"
status=$?
set -e
[[ "$status" == 101 ]]
printf '1\nbad\n' > "$TMP/inferred-value-error-order.expected"
cmp "$TMP/inferred-value-error-order.expected" "$TMP/inferred-value-error-order.out"
grep -q 'NUMERIC_PARSE' "$TMP/inferred-value-error-order.err"
cat > "$TMP/ordinary-union-negative.qui" <<'QUI'
(int | real64)[] input := [int(1)]
generated := [for value := input[] then value]
real64[] scale := [1.0]
result := generated * scale
QUI
cat > "$TMP/error-multisuccess-negative.qui" <<'QUI'
(int | real64 | error)[] input := [int(1)]
generated := [for value := input[] then value]
real64[] scale := [1.0]
result := generated * scale
QUI
cat > "$TMP/error-none-negative.qui" <<'QUI'
(real64 | none | error)[] input := [none]
generated := [for value := input[] then value]
real64[] scale := [1.0]
result := generated * scale
QUI
cat > "$TMP/explicit-error-arithmetic-negative.qui" <<'QUI'
(real64 | error)[] generated := [for text := ["1"][] then real64(text)]
real64[] scale := [1.0]
result := generated * scale
QUI
for name in ordinary-union-negative error-multisuccess-negative error-none-negative explicit-error-arithmetic-negative; do
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.check" 2>&1; then
        echo "accepted invalid generated element union: $name" >&2
        exit 1
    fi
    grep -q 'TYPE_MISMATCH' "$TMP/$name.check"
done
cat > "$TMP/partial-class-negative.qui" <<'QUI'
class Payload()
    int slot
value := Payload()
generated := [for entry := [1][] then value]
QUI
if "$QUIDRA" check "$TMP/partial-class-negative.qui" > "$TMP/partial-class-negative.check" 2>&1; then
    echo "accepted partially initialized generated Class value" >&2
    exit 1
fi
grep -q 'UNINITIALIZED_ARGUMENT' "$TMP/partial-class-negative.check"
cat > "$TMP/class-field-dtype-control.qui" <<'QUI'
class Holder()
    values := [for code := [65][] then nat8(code)]
holder := Holder()
nat8[] required := holder.values
nat8[] shifted := holder.values + nat8(1)
bits := bin(holder.values)
println(required[0] == 65 and shifted[0] == 66 and bits.len() == 8)
QUI
"$QUIDRA" build "$TMP/class-field-dtype-control.qui" -o "$TMP/class-field-dtype-control"
cat > "$TMP/class-field-dtype-control.expected" <<'OUT'
true
OUT
"$TMP/class-field-dtype-control" > "$TMP/class-field-dtype-control.actual"
cmp "$TMP/class-field-dtype-control.expected" "$TMP/class-field-dtype-control.actual"
cat > "$TMP/fallible-complete-generated.qui" <<'QUI'
class Whole() | error
    int value := 7
constructed := [for index := [0][] then Whole()]
println(constructed[0].value)
class Complete(int value)
Complete | error make(int value)
    return Complete(value)
returned := [for index := [8][] then make(index)]
println(returned[0].value)
QUI
"$QUIDRA" build "$TMP/fallible-complete-generated.qui" -o "$TMP/fallible-complete-generated"
cat > "$TMP/fallible-complete-generated.expected" <<'OUT'
7
8
OUT
"$TMP/fallible-complete-generated" > "$TMP/fallible-complete-generated.actual"
cmp "$TMP/fallible-complete-generated.expected" "$TMP/fallible-complete-generated.actual"
cat > "$TMP/fallible-partial-constructor-negative.qui" <<'QUI'
class Partial() | error
    int value
items := [for index := [0][] then Partial()]
println(items[0].value)
QUI
cat > "$TMP/fallible-partial-return-negative.qui" <<'QUI'
class Partial()
    int value
Partial | error make()
    return Partial()
items := [for index := [0][] then make()]
println(items[0].value)
QUI
for name in fallible-partial-constructor-negative fallible-partial-return-negative; do
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.check" 2>&1; then
        echo "accepted partially initialized fallible generated Class: $name" >&2
        exit 1
    fi
    grep -q 'UNINITIALIZED_UNION_PAYLOAD' "$TMP/$name.check"
done
