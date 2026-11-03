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
        text += item.string()
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
for name in selection evaluation propagation; do
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
