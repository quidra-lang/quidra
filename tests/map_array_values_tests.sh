#!/usr/bin/env bash
set -euo pipefail
QUIDRA="${1:?usage: map_array_values_tests.sh /path/to/quidra}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/array-values.qui" <<'QUI'
map<string, int[]> groups
int[] numbers := [7, 3]
.groups.set("b", numbers)
match groups.get("b")
    int[] values
        println("{values[0]}:{values[1]}")
    none
        println("missing")
map<string, nat[]> counts
nat[] amounts := [2, 5]
.counts.set("a", amounts)
match counts.get("a")
    nat[] values
        println("{values[0]}:{values[1]}")
    none
        println("missing")
map<string, int[2]> fixed
int[2] row := [4, 9]
.fixed.set("row", row)
match fixed.get("row")
    int[2] values
        println("{values[0]}:{values[1]}")
    none
        println("missing")
map<string, int> totals
.totals.increment("sum", by = 3)
.totals.increment("sum")
match totals.get("sum")
    int value
        println("scalar={value}")
    none
        println("missing")
QUI
"$QUIDRA" run "$TMP/array-values.qui" > "$TMP/actual"
printf '7:3\n2:5\n4:9\nscalar=4\n' > "$TMP/expected"
diff -u "$TMP/expected" "$TMP/actual"

for kind in 'int[]' 'nat[]' 'int[2]'; do
    cat > "$TMP/no-array-increment.qui" <<QUI
map<string, $kind> groups
.groups.increment("key")
QUI
    if "$QUIDRA" check "$TMP/no-array-increment.qui" --json > "$TMP/error"; then
        echo "map<$kind> unexpectedly exposes increment" >&2
        exit 1
    fi
    grep -q 'UNKNOWN_MEMBER' "$TMP/error"
done
