#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
expect() {
    local name=$1 expected=$2 actual
    cat > "$TMP/$name.qui"
    for threads in 1 2 4; do
        actual="$(QUIDRA_CPU_THREADS=$threads "$QUIDRA" "$TMP/$name.qui")"
        [[ "$actual" == "$expected" ]] || { printf '%s: expected %s; got %s\n' "$name" "$expected" "$actual" >&2; exit 1; }
    done
}
rejects() {
    local name=$1
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.json"; then
        printf '%s unexpectedly accepted\n' "$name" >&2; exit 1
    fi
    python3 - "$TMP/$name.json" <<'PY'
import json,sys
assert any(d['code']=='PARALLEL_FOR' for d in json.load(open(sys.argv[1]))['diagnostics'])
PY
}
fails() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    for threads in 1 2 4; do
        local rc=0
        QUIDRA_CPU_THREADS=$threads "$QUIDRA" "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err" || rc=$?
        [[ $rc == 101 && ! -s "$TMP/$name.out" ]]
        grep -Fq "runtime error[$code]" "$TMP/$name.err"
    done
}
expect example $'0\n1\n2\n3\n4' <<'QUI'
int[5] a
parallel for x < 5
    a[x] = x
for int value := a[]
    print(value)
    print(NL)
QUI
"$QUIDRA" build "$TMP/example.qui" -o "$TMP/example-native"
[[ "$(QUIDRA_CPU_THREADS=4 "$TMP/example-native")" == $'0\n1\n2\n3\n4' ]]
expect bitmap 136 <<'QUI'
int[17] a
parallel for x < 17
    a[x] = x
int sum := 0
for int value := a[]
    sum += value
print(sum)
print(NL)
QUI
expect cow $'0\n0\n0\n0\n0\n0\n1\n2\n3\n4' <<'QUI'
int[] a := [0, 0, 0, 0, 0]
int[] original := a
parallel for x < 5
    a[x] = x
for int value := original[]
    print(value)
    print(NL)
for int value := a[]
    print(value)
    print(NL)
QUI
expect descending $'0\n1\n2\n3\n4' <<'QUI'
int[5] a
parallel for 5 > x >= 0
    a[x] = x
for int value := a[]
    print(value)
    print(NL)
QUI
expect endpoints $'0\n1\n20\n3\n40\n5\n60' <<'QUI'
int32[] a := [0, 1, 2, 3, 4, 5, 6]
parallel for 0 < int32 x <= 6 step 2
    a[x] = x * 10
for int32 value := a[]
    print(value)
    print(NL)
QUI
expect descending_step $'0\n1\n20\n3\n40\n5\n60' <<'QUI'
int32[] a := [0, 1, 2, 3, 4, 5, 6]
parallel for 8 > int32 x > 0 step -2
    a[x] = x * 10
for int32 value := a[]
    print(value)
    print(NL)
QUI
expect local_continue $'0\n10\n0\n30\n0' <<'QUI'
int32[] a := [0, 0, 0, 0, 0]
parallel for int32 x < 5
    if x % 2 == 0
        continue
    int32 result := x * 10
    a[x] = result
for int32 value := a[]
    print(value)
    print(NL)
QUI
expect pure_call $'0\n1\n4\n9' <<'QUI'
int32 square(int32 value)
    return value * value
int32[4] a
parallel for int32 x < 4
    a[x] = square(x)
for int32 value := a[]
    print(value)
    print(NL)
QUI
expect inner_break 0 <<'QUI'
parallel for int32 x < 7
    for int32 y < 3
        if y == 1
            break
        int32 result := x + y
print(int(0))
print(NL)
QUI
# Dynamic source calls run once, before scheduling, in written order.
expect snapshots $'1\n2\n9' <<'QUI'
int32[] source(int32 id, int32[] &values)
    print(id)
    print(NL)
    return values
int32[] left := [2, 4]
int32[] right := [3, 5]
parallel for int32 price := source(1, &left)[], int32 quantity := source(2, &right)[]
    int32 result := price * quantity
print(int(9))
print(NL)
QUI
expect snapshot_before_next_source $'1\n2\n1000' <<'QUI'
int32[] first(int32[] values)
    print(int(1))
    print(NL)
    return values
int32[] second(int32[] &prices, int32[] values)
    prices[0] = 1000
    print(int(2))
    print(NL)
    return values
int32[] prices := [2, 4]
int32[] quantities := [3, 5]
parallel for int32 price := first(prices)[], int32 quantity := second(&prices, quantities)[]
    int32 proof := quantity / (price - 1000)
print(prices[0])
print(NL)
QUI
expect empty 0 <<'QUI'
int32[] left := []
int32[] right := []
parallel for int32 price := left[], int32 quantity := right[]
    int32 result := price + quantity
parallel for 4 <= int32 x < 4
    int32 result := x / (x - 4)
print(int(0))
print(NL)
QUI
fails mismatched FOR_LENGTH <<'QUI'
int32[] source(int count)
    int32[] values := []
    for int i < count
        values = values.append(1)
    return values
parallel for int32 price := source(1)[], int32 quantity := source(0)[]
    int32 result := price / quantity
QUI
# The range values remain exact; only the small count crosses the scheduler ABI.
expect exact_values 0 <<'QUI'
int zero(int value)
    return value - value
parallel for 100000000000000000000 <= int x <= 100000000000000000002
    if x < 100000000000000000000 or x > 100000000000000000002
        int invalid_mapping := 1 / zero(x)
    int result := x - 100000000000000000000
print(int(0))
print(NL)
QUI
fails huge_count NUMERIC_CONVERSION <<'QUI'
parallel for 0 <= x <= 9223372036854775807
    int result := x
QUI
fails lowest_ordinal DIVISION_BY_ZERO <<'QUI'
parallel for int32 x < 5
    if x == 1
        int32 first := 1 / (x - 1)
    if x == 3
        int32 later := 1 / (x - 3)
QUI
# A later completed failure cannot replace the first logical ordinal's site.
grep -Fq 'lowest_ordinal.qui:3:' "$TMP/lowest_ordinal.err"
rejects outer_break <<'QUI'
parallel for int32 x < 3
    break
QUI
rejects outer_return <<'QUI'
int f()
    parallel for int32 x < 3
        return x
    return 0
print(f())
QUI
rejects output_read <<'QUI'
int32[] a := [0, 0]
parallel for int32 x < 2
    a[x] = a[x] + 1
QUI
rejects scalar_write <<'QUI'
int32 total := 0
parallel for int32 x < 2
    total += x
QUI
rejects reference_header <<'QUI'
int32[] a := [0, 0]
parallel for int32 &x := &a[]
    x = 1
QUI
rejects resource_call <<'QUI'
parallel for int32 x < 2
    print(x)
QUI
rejects real_range <<'QUI'
parallel for 0.0 <= real32 x <= 1.0 step 0.5
    real32 result := x * x
QUI
rejects managed_element <<'QUI'
string[] a := ["first", "second"]
parallel for string text := a[]
    void
QUI

rejects existing_range_binding <<'QUI'
int32 x := 0
parallel for x < 3
    int32 result := x
QUI
rejects existing_zip_binding <<'QUI'
int32 x := 0
int32[] a := [1, 2]
parallel for int32 y := a[], x = a[]
    int32 result := x + y
QUI
rejects receiver_field_capture <<'QUI'
class Counter(int32 count)
    void run()
        parallel for int32 x < count
            int32 result := count
Counter value := Counter(2)
value.run()
QUI
rejects current_instance_capture <<'QUI'
class Counter counter(int32 count)
    void run()
        parallel for int32 x < count
            local Counter copy := counter
Counter value := Counter(2)
value.run()
QUI
rejects receiver_output <<'QUI'
class Buffer(int32[] values)
    void .fill()
        parallel for int32 x < 2
            values[x] = x
Buffer buffer := Buffer([0, 0])
buffer.fill()
QUI

rejects reference_alias <<'QUI'
int[] a := [0, 0]
int &alias := &a[0]
parallel for x < 2
    a[x] = x
QUI

fails boxed_previous PARALLEL_FOR <<'QUI'
int[] a := [100000000000000000000]
parallel for x < 1
    a[x] = x
QUI
