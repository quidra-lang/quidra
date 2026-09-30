#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
OPT="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

verify_and_run() {
    local name="$1"
    local expected="$2"
    local source="$TMP/$name.qui"
    local llvm="$TMP/$name.ll"

    "$QUIDRA" llvm "$source" > "$llvm"
    "$OPT" -passes=verify -disable-output "$llvm"

    local actual
    actual="$("$QUIDRA" run "$source")"
    if [[ "$actual" != "$expected" ]]; then
        printf 'backend regression %s failed\nexpected:\n%s\nactual:\n%s\n' \
            "$name" "$expected" "$actual" >&2
        exit 1
    fi
}

printf 'scope' > "$TMP/file-handle-cleanup.txt"
cat > "$TMP/file-handle-cleanup.qui" <<QUI
void | error consume(string path)
    file.Handle handle = try file.open(path)
    string text = try handle.read()
    print(text)
    print(NL)
    return void

auto | error result = consume("$TMP/file-handle-cleanup.txt")
match result
    void
        print("done")
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
"$QUIDRA" llvm "$TMP/file-handle-cleanup.qui" > "$TMP/file-handle-cleanup.ll"
"$OPT" -passes=verify -disable-output "$TMP/file-handle-cleanup.ll"
grep -Eq 'call void @quidra_managed_release\(ptr [^,]+, ptr @quidra_file_handle_drop\)' "$TMP/file-handle-cleanup.ll"
[[ "$("$QUIDRA" run "$TMP/file-handle-cleanup.qui")" == $'scope\ndone' ]]

# Regression: dynamic-array spare capacity may come from a recycled small managed
# allocation. A freshly exposed append slot must be empty before ArraySet replaces
# it, otherwise a stale managed pointer can be released as a live element.
cat > "$TMP/file-handle-string-append.qui" <<QUI
file.Handle create_file(string path)
    match file.create(path)
        file.Handle handle
            return handle
        error problem
            print("create failed")
            print(NL)
            process.exit(1)

void go()
    int state = 20270917
    file.Handle w = create_file("$TMP/file-handle-string-append.txt")
    string[] buffer = []
    for i in range(0, 3)
        state = (state * 48271) % 2147483647
        string[] fields = [i.string(), " ", state.string(), NL]
        string line = fields.join("")
        buffer = buffer.append(line)
    string joined = buffer.join("|")
    print("RESULT {joined.trim()}")
    print(NL)
    w.close()
    return

go()
QUI
expected_file_handle_append="$(printf 'RESULT 0 1392375122\n|1 1543813903\n|2 1610877166')"
verify_and_run file-handle-string-append "$expected_file_handle_append"

# The original defect reproduced in both AOT and ORC JIT. Keep the JIT path in
# the regression so a backend-specific lifetime regression cannot reappear.
rm -f "$TMP/file-handle-string-append.txt"
repl_file_handle_append="$("$QUIDRA" repl < "$TMP/file-handle-string-append.qui")"
[[ "$repl_file_handle_append" == *"$expected_file_handle_append"* ]]


cat > "$TMP/short-circuit.qui" <<'QUI'
int a = 4
if a % 2 == 0 and a % 4 == 0
    print("mod")
    print(NL)

if a + 1 == 5 and 12 / a == 3
    print("arith")
    print(NL)

if a == 4 or 10 / a == 0
    print("or")
    print(NL)
QUI
verify_and_run short-circuit "$(printf 'mod\narith\nor')"

cat > "$TMP/contextual-literals.qui" <<'QUI'
float x = float(3)
float32 y = float32(2)
float[] xs = [1.0, 2.0, 3.0]

float half(float value)
    return value / 2.0

float negative = -3.0
float mixed = 2.0 * 3.0
int8 small = 5
int8 sum = small + 100

print(x)
print(NL)
print(y)
print(NL)
print(xs[2])
print(NL)
print(half(3.0))
print(NL)
print(negative)
print(NL)
print(mixed)
print(NL)
print(sum)
print(NL)
QUI
verify_and_run contextual-literals "$(printf '3.0\n2.0\n3.0\n1.5\n-3.0\n6.0\n105')"

cat > "$TMP/bitwise.qui" <<'QUI'
uint8 a = 240
uint8 b = 204
print(a AND b)
print(NL)
print(a OR b)
print(NL)
print(a XOR b)
print(NL)
print(NOT a)
print(NL)
uint8 small = 3
print(small << 2)
print(NL)
uint8 high = 128
print(high >> 7)
print(NL)
int8 signed_value = -8
print(signed_value >> 2)
print(NL)
int8 signed_zero = 0
print(NOT signed_zero)
print(NL)
int8 signed_left = 64
print(signed_left << 1)
print(NL)
QUI
verify_and_run bitwise "$(printf '192\n252\n60\n15\n12\n1\n-2\n-1\n-128')"

cat > "$TMP/shift-count-runtime.qui" <<'QUI'
uint8 value = 1
uint8 count = 8
print(value << count)
print(NL)
QUI
set +e
"$QUIDRA" run "$TMP/shift-count-runtime.qui" >"$TMP/shift-count-runtime.out" 2>"$TMP/shift-count-runtime.err"
status=$?
set -e
[[ "$status" -eq 101 ]]
cat "$TMP/shift-count-runtime.out" "$TMP/shift-count-runtime.err" | grep -q "SHIFT_COUNT"

cat > "$TMP/string-concat-loop.qui" <<'QUI'
string text = ""
int i = 0
while i < 200000
    text = "a" + "b"
    i += 1
print(text)
print(NL)
QUI
"$QUIDRA" llvm "$TMP/string-concat-loop.qui" > "$TMP/string-concat-loop.ll"
"$OPT" -passes=verify -disable-output "$TMP/string-concat-loop.ll"
! grep -q 'alloca ptr, i64' "$TMP/string-concat-loop.ll"
[[ "$("$QUIDRA" run "$TMP/string-concat-loop.qui")" == "ab" ]]


cat > "$TMP/frame-scratch-loop.qui" <<'QUI'
tensor<float32> values = tensor.ones<float32>([2])
int i = 0
while i < 20
    int | error parsed = int.parse("42")
    tensor<float32> shifted = values + 1.0
    tensor<float32> first = shifted[0:1]
    values[0] = 2.0
    string joined = "x" + "y"
    i += 1
print(values[0].item())
print(NL)
QUI
"$QUIDRA" llvm "$TMP/frame-scratch-loop.qui" > "$TMP/frame-scratch-loop.ll"
"$OPT" -passes=verify -disable-output "$TMP/frame-scratch-loop.ll"
awk '/^define .* @main\(/,/^}$/' "$TMP/frame-scratch-loop.ll" > "$TMP/frame-scratch-main.ll"
first_loop_line="$(grep -n '^while\.' "$TMP/frame-scratch-main.ll" | head -n1 | cut -d: -f1)"
last_alloca_line="$(grep -n ' = alloca ' "$TMP/frame-scratch-main.ll" | tail -n1 | cut -d: -f1)"
[[ -n "$first_loop_line" && -n "$last_alloca_line" ]]
[[ "$last_alloca_line" -lt "$first_loop_line" ]]
[[ "$("$QUIDRA" run "$TMP/frame-scratch-loop.qui")" == "2.0" ]]

cat > "$TMP/float32-rounding.qui" <<'QUI'
float32 rounded = 0.000001
float source = 0.1
float32 narrowed = float32(source)
print(rounded > 0.0000009 and rounded < 0.0000011)
print(NL)
print(narrowed > 0.099 and narrowed < 0.101)
print(NL)
QUI
verify_and_run float32-rounding "$(printf 'true\ntrue')"

cat > "$TMP/float32-range-error.qui" <<'QUI'
float source = 1.0e100
float32 narrowed = float32(source)
print(narrowed)
print(NL)
QUI
set +e
ASAN_OPTIONS=detect_leaks=0 "$QUIDRA" run "$TMP/float32-range-error.qui" >"$TMP/float32-range-error.out" 2>"$TMP/float32-range-error.err"
status=$?
set -e
[[ "$status" -eq 101 ]]
cat "$TMP/float32-range-error.out" "$TMP/float32-range-error.err" | grep -q "UNHANDLED_ERROR"

cat > "$TMP/try-class.qui" <<'QUI'
class Pair
    int a
    int b

    construct(int a_value, int b_value)
        a = a_value
        b = b_value

Pair | error make(bool ok)
    if ok
        return Pair(2, 3)
    return error("bad")

int | error sum(bool ok)
    Pair pair = try make(ok)
    return pair.a + pair.b

auto | error result = sum(true)
match result
    int value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
verify_and_run try-class "5"

# Source provenance is metadata, not mutable runtime text. Each source location
# must lower to one pointer to an immutable record so ordinary statements do not
# copy four strings on the hot path. Preserve the same diagnostic payload when
# a runtime failure actually needs it.
cat > "$TMP/source-provenance-pointer.qui" <<'QUI'
int[] values = [1]
int index = 2
print(values[index])
print(NL)
QUI
"$QUIDRA" llvm "$TMP/source-provenance-pointer.qui" > "$TMP/source-provenance-pointer.ll"
"$OPT" -passes=verify -disable-output "$TMP/source-provenance-pointer.ll"
grep -Fq 'declare void @quidra_runtime_set_source_provenance(ptr)' "$TMP/source-provenance-pointer.ll"
! grep -Fq 'declare void @quidra_runtime_set_source_provenance(ptr, ptr' "$TMP/source-provenance-pointer.ll"
grep -Eq '^@\.quidra\.source\.[0-9]+ = private constant \{ ptr, ptr, ptr, ptr, i64, i64 \} \{ ptr @\.str\.' "$TMP/source-provenance-pointer.ll"
grep -Eq 'call void @quidra_runtime_set_source_provenance\(ptr @\.quidra\.source\.[0-9]+\)' "$TMP/source-provenance-pointer.ll"

set +e
"$QUIDRA" run "$TMP/source-provenance-pointer.qui" >"$TMP/source-provenance-pointer.out" 2>"$TMP/source-provenance-pointer.err"
status=$?
set -e
[[ "$status" -eq 101 ]]
grep -Fq 'source_revision=' "$TMP/source-provenance-pointer.err"
grep -Fq 'node_id=' "$TMP/source-provenance-pointer.err"
grep -Fq 'node_kind=' "$TMP/source-provenance-pointer.err"
grep -Fq 'source_file=' "$TMP/source-provenance-pointer.err"
