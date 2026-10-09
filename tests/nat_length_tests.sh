#!/usr/bin/env bash
# Lengths, counts, shapes, sizes and API index values are nat: the result
# types of every API of the nat list, checked nat subtraction against the
# signed form, index operands of every integer kind, nat parameters taking
# nat(n) for other integer kinds, and the old range(...) over a length.
set -euo pipefail
set -x
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"

cat > "$TMP/results.qui" <<'QUI'
string text = "héllo"
int64[] values = [3, 1, 2]
bin bits = bin.fill(5, 1)
nat a = len(text)
nat b = len(values)
nat c = len(bits)
print("{a} {b} {c}")
print(NL)
map.Map<string, int64> table = map.Map<string, int64>()
table.set("x", 1)
set.Set<int64> seen = set.Set<int64>()
seen.add(4)
seen.add(5)
nat d = table.size()
nat e = seen.size()
print("{d} {e}")
print(NL)
nat | none where = text.find("l")
match where
    nat position
        print(position)
    none
        print("none")
print(NL)
json.Value document = json.parse("[1, 2, 3]")
nat | error count = document.size()
match count
    nat items
        print(items)
    error problem
        print("error")
print(NL)
tensor<real32> grid = tensor.zeros<real32>([2, 3])
nat[] shape = grid.shape()
print("{shape[0]} {shape[1]}")
print(NL)
QUI
results_expected="$(printf '5 3 5\n1 2\n2\n3\n2 3')"
[[ "$("$QUIDRA" run "$TMP/results.qui")" == "$results_expected" ]]

# len(a) - 1 is a checked nat subtraction: on an empty array it stops, while
# the signed form int(len(a)) - 1 is -1.
cat > "$TMP/signed.qui" <<'QUI'
int64[] empty = []
print(int(len(empty)) - 1)
print(NL)
int64[] values = [7, 8, 9]
print(len(values) - 1)
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/signed.qui")" == "$(printf -- '-1\n2')" ]]

cat > "$TMP/underflow.qui" <<'QUI'
int64[] empty = []
print(len(empty) - 1)
QUI
set +e
"$QUIDRA" run "$TMP/underflow.qui" >"$TMP/underflow.out" 2>"$TMP/underflow.err"
underflow_rc=$?
set -e
[[ "$underflow_rc" -eq 101 ]]
grep -q 'INTEGER_OVERFLOW' "$TMP/underflow.err"
grep -q 'nat subtraction result is negative' "$TMP/underflow.err"

# Index operands accept every integer kind; old range(...) loops over a
# length keep compiling, their elements int.
cat > "$TMP/indices.qui" <<'QUI'
int64[] values = [10, 20, 30, 40]
int8 small = 1
nat16 medium = 2
int wide = 3
nat natural = 0
print("{values[small]} {values[medium]} {values[wide]} {values[natural]}")
print(NL)
string text = "abc"
print(text[small])
print(NL)
int64 total = 0
for i in range(len(values))
    total = total + values[i]
print(total)
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/indices.qui")" == "$(printf '20 30 40 10\nb\n100')" ]]

# A nat result in an int context and an argument of another integer kind at
# a nat parameter are TYPE_MISMATCH, naming the conversion to write.
printf 'int64[] values = [1, 2]\nint count = len(values)\n' > "$TMP/to-int.qui"
set +e
"$QUIDRA" check "$TMP/to-int.qui" >"$TMP/to-int.out" 2>&1
to_int_rc=$?
set -e
[[ "$to_int_rc" -ne 0 ]]
grep -q 'TYPE_MISMATCH' "$TMP/to-int.out"
grep -q "'len(values)' has type nat; convert explicitly: int(len(values))." "$TMP/to-int.out"

printf 'int n = 3\nint64[] values = array(n, fill = 0)\n' > "$TMP/to-nat.qui"
set +e
"$QUIDRA" check "$TMP/to-nat.qui" >"$TMP/to-nat.out" 2>&1
to_nat_rc=$?
set -e
[[ "$to_nat_rc" -ne 0 ]]
grep -q 'TYPE_MISMATCH' "$TMP/to-nat.out"
grep -q "'n' has type int; this parameter is nat: write nat(n)." "$TMP/to-nat.out"

cat > "$TMP/explicit.qui" <<'QUI'
int n = 3
int64[] values = array(nat(n), fill = 7)
print(len(values))
print(NL)
int32[] dims = [2, 2]
tensor<real32> grid = tensor.zeros<real32>(nat(dims))
print(grid.shape()[1])
print(NL)
print(string.repeat("a", nat(n)))
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/explicit.qui")" == "$(printf '3\n2\naaa')" ]]

# nat(n) of a negative value at a success-only nat parameter stops with
# status 101 (contextual fail-fast).
printf 'int n = -1\nint64[] values = array(nat(n), fill = 0)\nprint(len(values))\n' > "$TMP/negative.qui"
set +e
"$QUIDRA" run "$TMP/negative.qui" >"$TMP/negative.out" 2>"$TMP/negative.err"
negative_rc=$?
set -e
[[ "$negative_rc" -eq 101 ]]
