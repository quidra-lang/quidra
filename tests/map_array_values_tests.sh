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

# The scalar get-match idiom must remain a read-only query even when lowered
# directly to hash/find calls. Deleted entries keep their insertion-order gaps.
cat > "$TMP/const-native-get.qui" <<'QUI'
map<int64, int64> numbers
.numbers.set(int64(1), int64(10))
.numbers.set(int64(9), int64(90))
.numbers.set(int64(17), int64(170))
.numbers.remove(int64(9))
const map<int64, int64> snapshot := numbers
match snapshot.get(int64(17))
    int64 value
        println(value)
    none
        process.exit(1)
match snapshot.get(int64(9))
    int64 value
        process.exit(2)
    none
        println("missing")
int64[] keys := snapshot.keys()
int64[] values := snapshot.values()
println("{keys[0]}:{keys[1]}={values[0]}:{values[1]}")
match numbers.get(int64(9))
    int64 value
        process.exit(3)
    none
        .numbers.set(int64(9), int64(900))
.numbers.set(int64(1), int64(11))
.numbers.set(int64(1), int64(12))
.numbers.increment(int64(1), by = int64(3))
match numbers.get(int64(1))
    int64 value
        println(value)
    none
        process.exit(4)
match snapshot.get(int64(1))
    int64 value
        println(value)
    none
        process.exit(5)
int64[] updated_keys := numbers.keys()
println("{updated_keys[0]}:{updated_keys[1]}:{updated_keys[2]}")
println("{snapshot.size()}:{numbers.size()}")
QUI
"$QUIDRA" run "$TMP/const-native-get.qui" > "$TMP/actual"
printf '170\nmissing\n1:17=10:170\n15\n10\n1:17:9\n2:3\n' > "$TMP/expected"
diff -u "$TMP/expected" "$TMP/actual"
"$QUIDRA" ir "$TMP/const-native-get.qui" > "$TMP/native-get.ir"
python3 - "$TMP/native-get.ir" <<'PY'
import sys
entry = open(sys.argv[1]).read().split("function $entry()", 1)[1]
assert "map.match.found" in entry, "native scalar get-match idiom was lost"
assert ".__find(" in entry, "native scalar lookup no longer uses hash probing"
assert "field.set " not in entry, "get-match writes the map's hidden cache"
assert "array.set " not in entry, "get-match writes the map's hidden key cache"
PY

for action in set remove increment; do
    arguments='int64(1)'
    if [[ "$action" == set ]]; then arguments+=', int64(2)'; fi
    cat > "$TMP/const-native-write.qui" <<QUI
const map<int64, int64> snapshot := map<int64, int64>()
.snapshot.$action($arguments)
QUI
    if "$QUIDRA" check "$TMP/const-native-write.qui" --json > "$TMP/error"; then
        echo "const map unexpectedly permits $action" >&2
        exit 1
    fi
    grep -q 'WRITE_CAPABILITY' "$TMP/error"
done
