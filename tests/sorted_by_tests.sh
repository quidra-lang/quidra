#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/quidra-sorted-by-cache"

cat > "$TMP/sort-by.qui" <<'QUI'
string[] names = ["Aki", "Yui", "Haru"]
int[] scores = [90, 85, 95]
auto ranked = names.sorted(by = scores, descending = true)
string[] expected_ranked = ["Haru", "Aki", "Yui"]
print(ranked == expected_ranked)
string[] original_names = ["Aki", "Yui", "Haru"]
print(names == original_names)
int[] original_scores = [90, 85, 95]
print(scores == original_scores)

string[] tied = ["Zoe", "Ada", "Leo", "Bob"]
int[] points = [5, 10, 10, 5]
auto ranked_tied = tied.sorted(by = (points, tied), descending = (true, false))
string[] expected_tied = ["Ada", "Leo", "Bob", "Zoe"]
print(ranked_tied == expected_tied)
string[] ids = ["first", "second", "third", "fourth"]
int[] all_equal = [2, 2, 2, 2]
print(ids.sorted(by = all_equal) == ids)
print(ids.sorted(by = all_equal, descending = true) == ids)
string[] expected_names_desc = ["third", "second", "fourth", "first"]
print(ids.sorted(by = (all_equal, ids), descending = true) == expected_names_desc)

bool descending = true
string[] expected_natural_desc = ["Yui", "Haru", "Aki"]
print(names.sorted(descending = descending) == expected_natural_desc)
print(names.sorted(by = scores, descending = descending) == expected_ranked)

int[] empty = []
int[] no_keys = []
print(empty.sorted(by = no_keys) == empty)
int[] singleton = [7]
int[] single_keys = [1]
print(singleton.sorted(by = single_keys) == singleton)
QUI
[[ "$("$QUIDRA" "$TMP/sort-by.qui")" == "truetruetruetruetruetruetruetruetruetruetrue" ]]

cat > "$TMP/sort-by-bad-length.qui" <<'QUI'
int[] values = [1, 2]
int[] keys = [1]
print(values.sorted(by = keys))
QUI
if "$QUIDRA" "$TMP/sort-by-bad-length.qui" >"$TMP/bad-length.out" 2>"$TMP/bad-length.err"; then
    echo "sorted(by=...) incorrectly accepted key length mismatch" >&2
    exit 1
fi
cat > "$TMP/sort-by-fixed-mismatch.qui" <<'QUI'
int[2] values = [1, 2]
int[3] keys = [1, 2, 3]
print(values.sorted(by = keys))
QUI
if "$QUIDRA" check "$TMP/sort-by-fixed-mismatch.qui" >"$TMP/fixed-mismatch.out" 2>"$TMP/fixed-mismatch.err"; then
    echo "sorted(by=...) accepted statically mismatched fixed array lengths" >&2
    exit 1
fi
cat > "$TMP/sort-by-wrong-flag.qui" <<'QUI'
string[] a = ["a", "b"]
int[] k = [1, 2]
print(a.sorted(by = (k, a), descending = (true, false, true)))
QUI
if "$QUIDRA" check "$TMP/sort-by-wrong-flag.qui" >"$TMP/wrong-flag.out" 2>"$TMP/wrong-flag.err"; then
    echo "sorted(by=...) accepted mismatched descending tuple" >&2
    exit 1
fi
cat > "$TMP/sort-by-wrong-key.qui" <<'QUI'
string[] values = ["a", "b"]
string[][] keys = [["a"], ["b"]]
print(values.sorted(by = keys))
QUI
if "$QUIDRA" check "$TMP/sort-by-wrong-key.qui" >"$TMP/wrong-key.out" 2>"$TMP/wrong-key.err"; then
    echo "sorted(by=...) accepted unorderable nested key type" >&2
    exit 1
fi
