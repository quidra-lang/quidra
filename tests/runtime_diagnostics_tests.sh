#!/usr/bin/env bash
# The canonical runtime failure messages (docs/spec/diagnostics.md, "Runtime
# failure format" and the coded runtime failures): one program per row, each
# run from a scratch copy of its source, so the report shows the snippet.
# Each row checks the exit status, that the output written before the
# failure was flushed, and the report's fields (code, file, line, column,
# snippet, message) through tests/runtime_report.py.
set -euo pipefail

QUIDRA="$1"
HELPER="$(cd "$(dirname "$0")" && pwd)/runtime_report.py"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# row NAME STATUS LINE COLUMN MESSAGE [field=value ...] < PROGRAM: builds the
# program read from stdin as $TMP/NAME.qui, runs it from $TMP and checks its
# status, that its standard output is exactly "before", and the report: CODE
# from the field list (INDEX_BOUNDS when absent), the root file, LINE,
# COLUMN, the snippet and MESSAGE.
row() {
    local name=$1 status=$2 line=$3 column=$4 message=$5
    shift 5
    local code=INDEX_BOUNDS
    local fields=()
    for field in "$@"; do
        case "$field" in
            code=*) code=${field#code=} ;;
            *) fields+=("$field") ;;
        esac
    done
    cat > "$TMP/$name.qui"
    if ! "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" > "$TMP/$name.build" 2>&1; then
        fail "$name: build failed"; cat "$TMP/$name.build"; return
    fi
    local rc=0
    (cd "$TMP" && perl -e 'alarm shift; exec @ARGV' 60 "./$name" > "$TMP/$name.out" 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne "$status" ]]; then
        fail "$name: exit status $rc, expected $status"; cat "$TMP/$name.err"; return
    fi
    if [[ "$(cat "$TMP/$name.out")" != before ]]; then
        fail "$name: the output before the failure was not flushed"; cat "$TMP/$name.out"; return
    fi
    if ! python3 "$HELPER" "$TMP/$name.err" "code=$code" "file=$TMP/$name.qui" "line=$line" \
            "column=$column" snippet=yes "message=$message" ${fields[@]+"${fields[@]}"}; then
        fail "$name"; return
    fi
    echo "ok   $name"
}

# --- Indexes: an element index failure names the index and the length and
# is reported at the index operand; a slice names its bounds and is reported
# at the indexing expression; a tensor access names the axis.
row array-past-end 101 4 14 "index 3 out of bounds for length 3" caret=14 <<'QUI'
int[] values = [1, 2, 3]
nat at = len(values)
print("before" + NL)
print(values[at])
QUI
row array-negative 101 4 14 "index -1 out of bounds for length 3" <<'QUI'
int[] values = [1, 2, 3]
int at = int(len(values))
print("before" + NL)
print(values[at - 4])
QUI
row array-write 101 4 8 "index 3 out of bounds for length 3" <<'QUI'
int[] values = [1, 2, 3]
nat at = len(values)
print("before" + NL)
values[at] = 9
QUI
row fixed-array 101 4 13 "index 5 out of bounds for length 4" <<'QUI'
int[4] slots = [1, 2, 3, 4]
nat at = len(slots) + 1
print("before" + NL)
print(slots[at])
QUI
row string-ascii 101 4 12 "index 6 out of bounds for length 6" <<'QUI'
string word = "quidra"
nat at = len(word)
print("before" + NL)
print(word[at])
QUI
row string-non-ascii 101 4 12 "index 3 out of bounds for length 3" <<'QUI'
string word = "日本語"
nat at = len(word)
print("before" + NL)
print(word[at])
QUI
row string-negative 101 4 12 "index -1 out of bounds for length 6" <<'QUI'
string word = "quidra"
int at = int(len(word))
print("before" + NL)
print(word[at - 7])
QUI
row string-compare 101 4 9 "index 6 out of bounds for length 6" <<'QUI'
string word = "quidra"
nat at = len(word)
print("before" + NL)
if word[at] == "q"
    print("q")
QUI
row bin-get 101 4 12 "index 10 out of bounds for length 8" <<'QUI'
bin data = bin.fill(8, 0)
nat at = len(data) + 2
print("before" + NL)
print(data[at])
QUI
row bin-set 101 4 6 "index 10 out of bounds for length 8" <<'QUI'
bin data = bin.fill(8, 0)
nat at = len(data) + 2
print("before" + NL)
data[at] = bin.fill(1, 1)
QUI
row bin-slice 101 4 7 "slice [2, 10) out of bounds for length 8" <<'QUI'
bin data = bin.fill(8, 0)
nat at = len(data) + 2
print("before" + NL)
print(data[2:at])
QUI
row text-slice-past-end 101 4 7 "slice [1, 9) out of bounds for length 6" <<'QUI'
string word = "quidra"
nat stop = len(word) + 3
print("before" + NL)
print(word.slice(1, int(stop)))
QUI
row text-slice-reversed 101 4 7 "slice [4, 2) out of bounds for length 6" <<'QUI'
string word = "quidra"
int at = int(len(word))
print("before" + NL)
print(word.slice(4, at - 4))
QUI
row tensor-index 101 4 7 "index 3 out of bounds for axis 1 with length 3" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3])
int at = 3
print("before" + NL)
print(grid[1, at].item())
QUI
row tensor-set 101 4 1 "index 2 out of bounds for axis 0 with length 2" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3])
int at = 3
print("before" + NL)
grid[at - 1, 0] = real32(6)
QUI
row tensor-slice 101 4 23 "slice [0, 3) out of bounds for axis 0 with length 2" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3])
int at = 3
print("before" + NL)
tensor<real32> part = grid[0:at, 0]
QUI

# --- Recursion: the limit, at the call that would exceed it, for a function
# recursive only through itself (its depth argument), mutual recursion and
# recursion through an fn value (the call-depth counter).
row recursion-self 101 4 12 "maximum recursion depth exceeded (limit 4096)" code=CALL_DEPTH_LIMIT <<'QUI'
int down(int n)
    if n < 0
        return 0
    return down(n + 1) + 1
print("before" + NL)
print(down(0))
QUI
row recursion-mutual 101 9 12 "maximum recursion depth exceeded (limit 4096)" code=CALL_DEPTH_LIMIT <<'QUI'
int pong(int n);

int ping(int n)
    if n < 0
        return 0
    return pong(n + 1) + 1

int pong(int n)
    return ping(n + 1) + 1

print("before" + NL)
print(ping(0))
QUI
row recursion-fn-value 101 4 12 "maximum recursion depth exceeded (limit 4096)" code=CALL_DEPTH_LIMIT <<'QUI'
int again(fn<int>(int) f, int n)
    if n < 0
        return 0
    return f(n + 1) + 1

int step(int n)
    return again(step, n)

print("before" + NL)
print(step(0))
QUI

# --- Conversions: the destination type and the reason. A conversion that may
# fail has an error alternative; a success-only use stops at the consuming
# expression with that error.
conversion_row() {
    local name=$1 line=$2 column=$3 message=$4
    row "$name" 101 "$line" "$column" "$message" code=UNHANDLED_ERROR
}
conversion_row conversion-int8-high 4 7 "numeric conversion out of range: value cannot be represented as int8" <<'QUI'
int value = 300
int8 fits = int8(value - 200)
print("before" + NL)
print(int8(value))
QUI
conversion_row conversion-int8-low 3 7 "numeric conversion out of range: value cannot be represented as int8" <<'QUI'
int value = -300
print("before" + NL)
print(int8(value))
QUI
conversion_row conversion-nat64-int32 3 7 "numeric conversion out of range: value cannot be represented as int32" <<'QUI'
nat64 value = 1099511627776
print("before" + NL)
print(int32(value))
QUI
conversion_row conversion-negative-nat8 3 7 "numeric conversion out of range: value cannot be represented as nat8" <<'QUI'
int value = -1
print("before" + NL)
print(nat8(value))
QUI
conversion_row conversion-real32-finite 3 7 "numeric conversion out of range: value cannot be represented as real32" <<'QUI'
real64 value = 1.0e300
print("before" + NL)
print(real32(value))
QUI
conversion_row conversion-real32-infinite 4 7 "numeric conversion failed: non-finite value cannot be represented as real32" <<'QUI'
real64 zero = 0.0
real64 value = 1.0 / zero
print("before" + NL)
print(real32(value))
QUI
conversion_row conversion-bigint-int32 3 7 "numeric conversion out of range: value cannot be represented as int32" <<'QUI'
int value = 1099511627776
print("before" + NL)
print(int32(value))
QUI
conversion_row conversion-real-not-integral 3 7 "numeric conversion failed: value is not an integer and cannot be represented as int" <<'QUI'
real value = 1.5
print("before" + NL)
print(int(value))
QUI
conversion_row conversion-real-real64 5 7 "numeric conversion out of range: value cannot be represented as real64" <<'QUI'
int ten = 10
int big = ten ^ int(400)
real value = real(big)
print("before" + NL)
print(real64(value))
QUI
conversion_row conversion-array-element 3 20 "numeric conversion out of range: array element cannot be represented as int8" <<'QUI'
int[] values = [1, 300]
print("before" + NL)
int8[] converted = int8(values)
QUI
conversion_row conversion-tensor-element 4 26 "numeric conversion out of range: tensor element cannot be represented as int8" <<'QUI'
tensor<int64> values = tensor.ones<int64>([2])
values[1] = 300
print("before" + NL)
tensor<int8> converted = int8(values)
QUI

if [[ "$failures" -ne 0 ]]; then
    echo "$failures runtime diagnostics row(s) failed"
    exit 1
fi
echo "runtime diagnostics tests passed"
