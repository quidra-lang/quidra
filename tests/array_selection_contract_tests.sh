#!/usr/bin/env bash
# Selections allocate owned temporaries and copy each selected row independently.
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/selection-temporaries.qui" <<'QUI'
int[] values = [10, 20, 30]
int[] signed_indices = [2, 0, 2]
nat[] natural_indices = [0, 2]
bool[] mask = [false, true, false]
print(len(values[signed_indices]) == 3)
print(NL)
print(len(values[natural_indices]) == 2)
print(NL)
print(len(values[mask]) == 1)
print(NL)
QUI

# A selection consumed by len has no escaping owner. Its allocated output
# must be released; checking the actual IR catches leaks without RSS noise.
"$QUIDRA" ir "$TMP/selection-temporaries.qui" > "$TMP/selection-temporaries.ir"
python3 - "$TMP/selection-temporaries.ir" <<'PY'
import re
import sys

text = open(sys.argv[1]).read()
entry = text.split("function $entry(", 1)[1].split("\nend\n", 1)[0]
selections = re.findall(r"^\s*%(\d+) = array\.alloc %\d+\s*$", entry, re.M)
assert len(selections) == 3, selections
for value in selections:
    assert re.search(rf"^\s*release %{value} : int\[\]\s*$", entry, re.M), \
        f"selection allocation %{value} has no owner or release"
PY

expected="$(printf 'true\n%.0s' {1..3})"
[[ "$("$QUIDRA" "$TMP/selection-temporaries.qui")" == "$expected" ]]

cat > "$TMP/selection-row-copies.qui" <<'QUI'
int[][] rows = [[1, 2], [3, 4]]
int[] repeated = [0, 0]
int[][] gathered = rows[repeated]
gathered[0][0] = 99
print(rows[0][0] == 1 and gathered[1][0] == 1)
print(NL)
bool[] mask = [true, false]
int[][] selected = rows[mask]
selected[0][1] = 77
print(rows[0][1] == 2 and selected[0][1] == 77)
print(NL)
// Taking a managed row from a temporary selection must preserve that row
// while releasing both the selection and the original temporary source.
int[][] make_rows()
    return [[5, 6], [7, 8]]
int[] repeated_indices()
    return [0, 0]
int[] row = make_rows()[repeated_indices()][0]
print(row[0] == 5 and row[1] == 6)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/selection-row-copies.qui")" == "$expected" ]]

cat > "$TMP/effectful-selection.qui" <<'QUI'
int[] replace_and_indices(int[] &values)
    values = [90, 80]
    values = [70, 60]
    values = [50, 40]
    return [1, 0]

bool[] replace_and_mask(int[] &values)
    values = [90, 80]
    values = [70, 60]
    values = [50, 40]
    return [false, true]

int[] mutate_and_indices(int[] &values)
    values[0] = 99
    return [0]

int mutate_and_index(int[] &values)
    values[1] = 60
    return 1

int[] source = [10, 20]
int[] gathered = source[replace_and_indices(&source)]
print(gathered[0] == 20 and gathered[1] == 10 and source[0] == 50)
print(NL)
source = [10, 20]
int[] selected = source[replace_and_mask(&source)]
print(selected[0] == 20 and source[0] == 50)
print(NL)
source = [10, 20]
int[] snapshot = source[mutate_and_indices(&source)]
print(snapshot[0] == 10 and source[0] == 99)
print(NL)
source = [10, 18446744073709551616]
int element = source[mutate_and_index(&source)]
print(element == 18446744073709551616 and source[1] == 60)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/effectful-selection.qui")" == "$(printf 'true\n%.0s' {1..4})" ]]

cat > "$TMP/effectful-array-operator.qui" <<'QUI'
int[] replace_and_values(int[] &values)
    values = [90, 80]
    values = [70, 60]
    values = [50, 40]
    return [1, 2]

int[] mutate_and_values(int[] &values)
    values[0] = 99
    return [1, 2]

int[] source = [10, 20]
int[] sum = source + replace_and_values(&source)
print(sum[0] == 11 and sum[1] == 22 and source[0] == 50)
print(NL)
source = [10, 20]
int[] snapshot = source + mutate_and_values(&source)
print(snapshot[0] == 11 and snapshot[1] == 22 and source[0] == 99)
print(NL)
source = [10, 20]
int[] owned_sum = [10, 20] + mutate_and_values(&source)
print(owned_sum[0] == 11 and owned_sum[1] == 22 and source[0] == 99)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/effectful-array-operator.qui")" == "$expected" ]]

cat > "$TMP/selection-try-cleanup.qui" <<'QUI'
int[] | error failed_selection()
    return error("selection failed")

int[] make_values()
    return [10, 20]

int[] | error snapshot_add_borrowed(const int[] values)
    return values + try failed_selection()

int[] | error snapshot_add_owned()
    return make_values() + try failed_selection()

int[] | error snapshot_select_borrowed(const int[] values)
    return values[try failed_selection()]

int[] | error snapshot_select_owned()
    return make_values()[try failed_selection()]

int[] values = [10, 20]
int[] | error first = snapshot_add_borrowed(values)
int[] | error second = snapshot_add_owned()
int[] | error third = snapshot_select_borrowed(values)
int[] | error fourth = snapshot_select_owned()
QUI

# A try error returns before the ordinary final release. The earlier
# managed operand must already belong to an owned local, which return
# cleanup visits. The successful path must move it out before release so
# the same value is not released again during function cleanup.
"$QUIDRA" ir "$TMP/selection-try-cleanup.qui" > "$TMP/selection-try-cleanup.ir"
python3 - "$TMP/selection-try-cleanup.ir" <<'PY'
import re
import sys

text = open(sys.argv[1]).read()
checked = set()
for name, body in re.findall(r"^function (snapshot_(?:add|select)_(?:borrowed|owned))"
                             r"(?:\.shape_checked)?\([^\n]*\n(.*?)\nend", text, re.M | re.S):
    call = re.search(r"^\s*%\d+ = call failed_selection\(", body, re.M)
    if not call:
        continue  # Shape-checking wrappers contain no operand evaluation.
    before_try = body[:call.start()]
    owned_slots = re.findall(r"^\s*local (\S+) : int\[\]\s*$", before_try, re.M)
    assert len(owned_slots) == 1, (name, owned_slots)
    slot = re.escape(owned_slots[0])
    stored = re.search(rf"^\s*store {slot}, %(\d+)\s*$", before_try, re.M)
    assert stored, f"{name}: earlier operand has no owned local before try"
    value = stored[1]
    producer = "clone" if name.endswith("borrowed") else "call make_values"
    assert re.search(rf"^\s*%{value} = {producer}\b", before_try, re.M), name
    moved = re.search(rf"^\s*%(\d+) = load\.move {slot}\s*$", body, re.M)
    assert moved, f"{name}: successful path leaves earlier operand in its local"
    assert re.search(rf"^\s*release %{moved[1]} : int\[\]\s*$", body, re.M), name
    checked.add(name)
assert len(checked) == 4, checked
PY
[[ -z "$("$QUIDRA" "$TMP/selection-try-cleanup.qui")" ]]

cat > "$TMP/literal-array-operators.qui" <<'QUI'
int[] sum = [1, 2] + [3, 4]
print(sum[0] == 4 and sum[1] == 6)
print(NL)
bool[] ordered = [1, 4] < [2, 3]
print(ordered[0] and not ordered[1])
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/literal-array-operators.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/array-comparison-chain.qui" <<'QUI'
int[] first = [1]
int[] second = [1]
int[] third = [1]
print(first == second == third)
QUI
if "$QUIDRA" check "$TMP/array-comparison-chain.qui" --json > "$TMP/array-comparison-chain.json"; then
    echo "array equality comparison chain was accepted" >&2
    exit 1
fi
python3 - "$TMP/array-comparison-chain.json" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1]))
assert any(item["code"] == "CHAINED_COMPARISON" for item in report["diagnostics"]), report
PY

echo "array selection ownership, independent copies and operator contracts passed"
