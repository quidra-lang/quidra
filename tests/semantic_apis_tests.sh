#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/quidra-semantic-apis-cache"

cat > "$TMP/group.qui" <<'QUI'
int value := 123456789
int negative := -1234567
int small := 12
real64 decimal := 12345.75
print("{value::3}|{negative::3}|{small::3}|{decimal:.2:3}|{value:012.:3}|{value:12.:3}")
QUI
[[ "$("$QUIDRA" "$TMP/group.qui")" == "123,456,789|-1,234,567|12|12,345.75|000,123,456,789|   123,456,789" ]]

cat > "$TMP/distinct.qui" <<'QUI'
distinct<int> first
.first.add(3)
.first.add(1)
.first.add(2)
.first.add(3)
distinct<int> second
.second.add(2)
.second.add(4)
.second.add(3)
joined := first.union(second)
both := first.intersection(second)
left := first.difference(second)
either := first.symmetric_difference(second)
int[] expected_joined := [3, 1, 2, 4]
int[] expected_both := [3, 2]
int[] expected_left := [1]
int[] expected_either := [1, 4]
print(joined.values() == expected_joined)
print(both.values() == expected_both)
print(left.values() == expected_left)
print(either.values() == expected_either)
print(first.size() == 3)
print(second.size() == 3)
distinct<int> empty
sole := first.union(empty)
nothing := first.intersection(empty)
print(sole.values() == first.values())
print(nothing.size() == 0)
QUI
[[ "$("$QUIDRA" "$TMP/distinct.qui")" == "truetruetruetruetruetruetruetrue" ]]

for invalid in ':0' ':1001' ':3:2'; do
    cat > "$TMP/group-invalid.qui" <<QUI
int value := 123456
print("{value:$invalid}")
QUI
    if "$QUIDRA" check "$TMP/group-invalid.qui" >"$TMP/group-invalid.out" 2>"$TMP/group-invalid.err"; then
        echo "Invalid grouping format was accepted: $invalid" >&2
        exit 1
    fi
done
cat > "$TMP/group-string.qui" <<'QUI'
string text := "hello"
print("{text::3}")
QUI
if "$QUIDRA" check "$TMP/group-string.qui" >"$TMP/group-string.out" 2>"$TMP/group-string.err"; then
    echo "Grouping of a string was accepted" >&2
    exit 1
fi
