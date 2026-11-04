#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
expect() {
    local name=$1 expected=$2 actual
    cat > "$TMP/$name.qui"
    actual="$("$QUIDRA" "$TMP/$name.qui")"
    [[ "$actual" == "$expected" ]] || { printf '%s: expected %s; got %s\n' "$name" "$expected" "$actual" >&2; exit 1; }
}
rejects() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.json"; then
        printf '%s unexpectedly accepted\n' "$name" >&2; exit 1
    fi
    python3 - "$TMP/$name.json" "$code" <<'PY'
import json,sys
assert any(d['code']==sys.argv[2] for d in json.load(open(sys.argv[1]))['diagnostics'])
PY
}
fails() {
    local name=$1 expected=$2 code=$3 actual rc=0
    cat > "$TMP/$name.qui"
    actual="$("$QUIDRA" "$TMP/$name.qui" 2> "$TMP/$name.err")" || rc=$?
    [[ $rc == 101 && "$actual" == "$expected" ]]
    grep -Fq "runtime error[$code]" "$TMP/$name.err"
}
expect zip 80 <<'QUI'
int[] prices := [10, 20]
int[] quantities := [2, 3]
int total := 0
for int price := prices[], int quantity := quantities[]
    total += price * quantity
print(total)
print(NL)
QUI
# Also exercise the native backend with the identical source and oracle.
"$QUIDRA" build "$TMP/zip.qui" -o "$TMP/zip-native"
[[ "$("$TMP/zip-native")" == 80 ]]
expect three 380 <<'QUI'
int[] prices := [10, 20]
int[] quantities := [2, 3]
int[] weights := [4, 5]
int total := 0
for price := prices[], int quantity := quantities[], weight := weights[]
    total += price * quantity * weight
print(total)
print(NL)
QUI
expect ordered_snapshots $'80\n1\n2\n1000' <<'QUI'
int[] first(int[] &order, int[] &values)
    order = order.append(1)
    return values
int[] second(int[] &order, int[] &prices, int[] &values)
    order = order.append(2)
    prices[0] = 1000
    return values
int[] order := []
int[] prices := [10, 20]
int[] quantities := [2, 3]
int total := 0
for int price := first(&order, &prices)[], int quantity := second(&order, &prices, &quantities)[]
    total += price * quantity
print(total)
print(NL)
for int value := order[]
    print(value)
    print(NL)
print(prices[0])
print(NL)
QUI
expect managed_snapshots $'a!u\nb!v\nchanged' <<'QUI'
string[] left := ["a", "b"]
string[] right := ["u", "v"]
for string a := left[], string b := right[]
    left[1] = "changed"
    a = a + "!"
    print(a + b)
    print(NL)
print(left[1])
print(NL)
QUI
expect empty 0 <<'QUI'
int[] left := []
int[] right := []
int count := 0
for int a := left[], int b := right[]
    count += 1
print(count)
print(NL)
QUI
expect existing_discard $'2\n4\n10' <<'QUI'
int[] left := [1, 2]
int[] right := [3, 4]
int a
int b
int sum := 0
for a = left[], b = right[], _ = left[]
    sum += a + b
print(a)
print(NL)
print(b)
print(NL)
print(sum)
print(NL)
QUI
expect nested_control $'11\n22\n11' <<'QUI'
int[] left := [1, 2, 3]
int[] right := [10, 20, 30]
for int a := left[], int b := right[]
    if a == 3
        break
    for int c := left[], int d := right[]
        if c != a
            continue
        print(c + d)
        print(NL)
int first_sum(int[] x, int[] y)
    for int a := x[], int b := y[]
        return a + b
    return 0
print(first_sum(left, right))
print(NL)
QUI
# No body marker (and thus no counter increment) may precede FOR_LENGTH.
fails mismatch $'0\n1\n2' FOR_LENGTH <<'QUI'
int[] source(int[] &order, int id, int count)
    order = order.append(id)
    int[] result := []
    for int i < count
        result = result.append(i)
    print(id)
    print(NL)
    return result
int[] order := []
int count := 0
print(count)
print(NL)
for int a := source(&order, 1, 1)[], int b := source(&order, 2, 2)[]
    count += 1
    print("body")
    print(NL)
QUI
fails empty_mismatch 0 FOR_LENGTH <<'QUI'
int[] left := []
int[] right := [1]
int count := 0
print(count)
print(NL)
for int a := left[], int b := right[]
    count += 1
    print("body")
    print(NL)
QUI
fails uninitialized_empty before UNINITIALIZED <<'QUI'
int[] left := []
int[] right := []
int a
int b
print("before")
for a = left[], b = right[]
    print(a + b)
print(b)
QUI
rejects duplicate SHADOWING <<'QUI'
int[] values := [1]
for int item := values[], int item := values[]
    print(item)
QUI
rejects wrong_type TYPE_MISMATCH <<'QUI'
int[] values := [1]
for int item := values[], string other := values[]
    print(item)
QUI
rejects reference FOR_HEADER <<'QUI'
int[] values := [1]
for int &item := &values[], int other := values[]
    print(item)
QUI
rejects extra_reference FOR_HEADER <<'QUI'
int[] values := [1]
for int item := values[], &other := &values[]
    print(item)
QUI
rejects range FOR_HEADER <<'QUI'
int[] values := [1]
for int item < 1, int other := values[]
    print(item)
QUI
rejects bin FOR_HEADER <<'QUI'
int[] values := [1]
bin bits := bin.fill(1, 1)
for int item := values[], other := bits[]
    print(item)
QUI
rejects pattern FOR_HEADER <<'QUI'
(int, int)[] pairs := [(1, 2)]
int[] values := [1]
for (int a, int b) := pairs[], int other := values[]
    print(a)
QUI
rejects index FOR_HEADER <<'QUI'
int[] values := [1]
for int item := values[nat index], int other := values[]
    print(item)
QUI
# The old single-source loop remains a mutable value copy.
expect single $'11\n12\n1' <<'QUI'
int[] values := [1, 2]
for int item := values[]
    item += 10
    print(item)
    print(NL)
print(values[0])
print(NL)
QUI
expect generic 10 <<'QUI'
int add_pairs<T>(T[] left, T[] right)
    int total := 0
    for T a := left[], T b := right[]
        total += int(a) + int(b)
    return total
int[] left := [1, 2]
int[] right := [3, 4]
print(add_pairs<int>(left, right))
print(NL)
QUI
expect managed_return ab <<'QUI'
string first_pair(string[] left, string[] right)
    for string a := left[], string b := right[]
        return a + b
    return "empty"
string[] left := ["a", "c"]
string[] right := ["b", "d"]
print(first_pair(left, right))
print(NL)
QUI
expect header_propagation header-error <<'QUI'
string[] | error stop()
    return error("stop")
int | error visit()
    string[] left := ["a"]
    for string a := left[], string b := (try stop())[]
        print("body")
    return 0
match visit()
    int value
        print("unexpected")
    error failure
        print("header-error")
        print(NL)
QUI
expect body_propagation body-error <<'QUI'
int | error stop()
    return error("stop")
int | error visit()
    string[] left := ["a"]
    string[] right := ["b"]
    for string a := left[], string b := right[]
        int value := try stop()
        print(a + b)
    return 0
match visit()
    int value
        print("unexpected")
    error failure
        print("body-error")
        print(NL)
QUI
rejects traversal FOR_HEADER <<'QUI'
int[] values := [1]
for int item := values[], int other := reflect.each<int>(values)[]
    print(item)
QUI

expect tuple_values $'a:4\nb:6' <<'QUI'
(string, int)[] pairs := [("a", 1), ("b", 2)]
int[] quantities := [3, 4]
for (string, int) pair := pairs[], int quantity := quantities[]
    (string label, int number) := pair
    print("{label}:{number + quantity}{NL}")
QUI
rejects header_scope UNKNOWN_NAME <<'QUI'
int[] values := [1]
for int item := values[], int other := item[]
    print(other)
QUI
rejects body_scope UNKNOWN_NAME <<'QUI'
int[] values := [1]
for int item := values[], int other := values[]
    print(item)
print(other)
QUI
