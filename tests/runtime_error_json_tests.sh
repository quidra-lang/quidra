#!/usr/bin/env bash
# The machine-readable runtime report (docs/spec/diagnostics.md,
# "Machine-readable runtime reports"): with QUIDRA_ERROR_FORMAT=json each
# report is one line of JSON on stderr in place of the text, checked here
# against the schema's fields; the human text never carries the provenance.
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# json_row NAME STATUS PYTHON: builds $TMP/NAME.qui, runs it with
# QUIDRA_ERROR_FORMAT=json and runs PYTHON with `reports` (the parsed
# lines of stderr) and `root` ($TMP) defined.
json_row() {
    local name=$1 status=$2 check=$3 rc=0
    "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" > "$TMP/$name.build" 2>&1 ||
        { fail "$name: build failed"; cat "$TMP/$name.build"; return; }
    (cd "$TMP" && QUIDRA_ERROR_FORMAT=json "./$name" > "$TMP/$name.out" 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne "$status" ]]; then fail "$name: exit status $rc, expected $status"; cat "$TMP/$name.err"; return; fi
    if ! python3 - "$TMP/$name.err" "$TMP" "$check" <<'PY'
import json, sys
FIELDS = ["schema_version", "kind", "status", "code", "message", "details", "location",
          "provenance", "deferred_origin", "hops", "unhandled_at", "path", "path_complete",
          "causes"]
text = open(sys.argv[1], encoding="utf-8").read()
root = sys.argv[2]
lines = [line for line in text.split("\n") if line]
reports = [json.loads(line) for line in lines]
for report in reports:
    assert list(report) == FIELDS, list(report)
    assert report["schema_version"] == 1
    assert report["kind"] in ("runtime_error", "test_assertion")
    assert isinstance(report["details"], dict)
    assert report["hops"] == [] and report["path"] == [] and report["causes"] == []
    assert report["path_complete"] is True and report["unhandled_at"] is None
exec(sys.argv[3])
PY
    then fail "$name"; cat "$TMP/$name.err"; return; fi
    # The text of the same failure carries no provenance suffix.
    rc=0
    (cd "$TMP" && "./$name" > /dev/null 2> "$TMP/$name.text") || rc=$?
    if grep -q 'source_revision=' "$TMP/$name.text"; then fail "$name: provenance in the text"; return; fi
    echo "ok   $name"
}

# An index trap: its details, its location and the provenance.
cat > "$TMP/index.qui" <<'QUI'
int[] values = [1, 2, 3]
print(values[3])
QUI
json_row index 101 '
[report] = reports
assert report["kind"] == "runtime_error" and report["status"] == 101
assert report["code"] == "INDEX_BOUNDS"
assert report["message"] == "index 3 out of bounds for length 3"
assert report["details"] == {"index": 3, "length": 3}, report["details"]
location = report["location"]
assert location["file"] == root + "/index.qui" and location["line"] == 2 and location["column"] == 14
assert location["source_file"].endswith("/index.qui") and len(location["source_revision"]) == 64
assert report["provenance"]["node_kind"] == "expression_statement"
assert report["provenance"]["source_revision"] == location["source_revision"]
'

# A slice trap and a tensor index trap: their arguments by the holes' names.
cat > "$TMP/slice.qui" <<'QUI'
string word = "quidra"
nat stop = len(word) + 3
print(word.slice(1, int(stop)))
QUI
json_row slice 101 '
[report] = reports
assert report["message"] == "slice [1, 9) out of bounds for length 6"
assert report["details"] == {"start": 1, "end": 9, "length": 6}, report["details"]
'
cat > "$TMP/tensor-index.qui" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3])
int at = 3
print(grid[1, at].item())
QUI
json_row tensor-index 101 '
[report] = reports
assert report["message"] == "index 3 out of bounds for axis 1 with length 3"
assert report["details"] == {"index": 3, "axis": 1, "length": 3}, report["details"]
'

# The recursion limit: generated code's message, the limit as its detail.
cat > "$TMP/recursion.qui" <<'QUI'
int down(int n)
    return down(n + 1) + 1

print(down(0))
QUI
json_row recursion 101 '
[report] = reports
assert report["code"] == "CALL_DEPTH_LIMIT"
assert report["message"] == "maximum recursion depth exceeded (limit 4096)"
assert report["details"] == {"limit": 4096}, report["details"]
'

# A coded failure without details (generated code's own message).
cat > "$TMP/overflow.qui" <<'QUI'
int64 big = int64.parse("9223372036854775807")
int64 sum = big + 1
QUI
json_row overflow 101 '
[report] = reports
assert report["code"] == "INTEGER_OVERFLOW" and report["details"] == {}
assert report["message"] == "integer overflow"
assert report["location"]["line"] == 2
'

# An uncoded runtime-library failure: code null.
cat > "$TMP/uncoded.qui" <<'QUI'
string fill = "ab"
string text = string.repeat(fill, 3)
QUI
json_row uncoded 101 '
[report] = reports
assert report["code"] is None and report["details"] == {}
assert report["message"] == "string fill must contain exactly one Unicode code point"
assert report["location"]["line"] == 2 and report["location"]["column"] == 1
'

# A test assertion: kind test_assertion, status 1, no message.
cat > "$TMP/assertion.qui" <<'QUI'
int answer = 41
test.check(answer == 42)
QUI
json_row assertion 1 '
[report] = reports
assert report["kind"] == "test_assertion" and report["status"] == 1
assert report["code"] is None and report["message"] == "" and report["details"] == {}
assert report["location"]["line"] == 2 and report["location"]["column"] == 1
assert report["provenance"] is not None
'

if [[ "$failures" -ne 0 ]]; then
    echo "runtime_error_json_tests: $failures failed"
    exit 1
fi
echo "runtime_error_json_tests: passed"
