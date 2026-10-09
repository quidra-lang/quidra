#!/usr/bin/env bash
# Where runtime failures are reported and how the report reads
# (docs/spec/diagnostics.md, "Runtime failure format"): the header names the
# file, line and column ("Quidra runtime error[CODE] at FILE:L:C"); the
# source line and a caret follow when the compiled file is present and
# unchanged; the message stands inside the gutter. A failure inside package
# code (an installed package, or a file it reaches through its own quoted
# imports) is reported at the user's statement, never at a package-internal
# line; so is a failure read after a call into user code returned within
# the same statement. Each row checks its report's fields through
# tests/runtime_report.py.
set -euo pipefail

QUIDRA="$1"
HELPER="$(cd "$(dirname "$0")" && pwd)/runtime_report.py"
TMP="$(mktemp -d)"
trap 'chmod -R u+rwx "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# build NAME [quidra args...]: builds $TMP/NAME.qui into $TMP/NAME.
build() {
    local name=$1
    shift
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" "$@" \
        > "$TMP/$name.build" 2>&1 || { cat "$TMP/$name.build"; return 1; }
}

# check_run NAME STATUS [field=value ...]: runs $TMP/NAME from $TMP and checks
# its exit status and its report's fields.
check_run() {
    local name=$1 status=$2
    shift 2
    local rc=0
    (cd "$TMP" && perl -e 'alarm shift; exec @ARGV' 60 "./$name" > "$TMP/$name.out" 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne "$status" ]]; then
        fail "$name: exit status $rc, expected $status"; cat "$TMP/$name.err"; return
    fi
    if ! python3 "$HELPER" "$TMP/$name.err" "$@"; then fail "$name"; return; fi
    echo "ok   $name"
}

# expect_failure NAME CODE LINE COLUMN STATUS [quidra args...]: builds and
# runs $TMP/NAME.qui and checks the code (TEST: a test assertion) and the
# root file's line and column its report names.
expect_failure() {
    local name=$1 code=$2 line=$3 column=$4 status=$5
    shift 5
    build "$name" "$@" || { fail "$name: build failed"; return; }
    if [[ "$code" == TEST ]]; then
        check_run "$name" "$status" kind=test_assertion "file=$TMP/$name.qui" "line=$line" "column=$column"
    else
        check_run "$name" "$status" "code=$code" "file=$TMP/$name.qui" "line=$line" "column=$column"
    fi
}

mkdir -p "$TMP/packages/loc_pkg"
cat > "$TMP/packages/loc_pkg/quidra.package" <<'MANIFEST'
name = loc_pkg
version = 0.1.0
MANIFEST
cat > "$TMP/packages/loc_pkg/main.qui" <<'QUI'
import internal = "./internal.qui"

extern int64 c_apply(fn<int64>(int64) callback, int64 value) = "location_test_apply"

int pick(int[] values, int i)
    int found = values[i]
    return found

int via_internal(int[] values, int i)
    return internal.pick(values, i)

int apply_then_fail(fn<int>(int) f, int[] values)
    int v = f(1)
    return values[v + 10]

int apply_native_then_fail(fn<int64>(int64) f, int[] values)
    int v = int(c_apply(f, 1))
    return values[v + 10]
QUI
cat > "$TMP/packages/loc_pkg/internal.qui" <<'QUI'
int pick(int[] values, int i)
    int found = values[i]
    return found
QUI
cat > "$TMP/apply.c" <<'C'
typedef long long (*location_test_callback)(long long);

long long location_test_apply(location_test_callback callback, long long value) {
    return callback(value);
}

long long location_test_apply_user(location_test_callback callback, long long value) {
    return callback(value);
}
C
"${CC:-cc}" -c "$TMP/apply.c" -o "$TMP/apply.o"

# An index failure inside a package function: the user's calling statement.
cat > "$TMP/package-index.qui" <<'QUI'
import loc = loc_pkg

int[] data = [1, 2, 3]
print(loc.pick(data, 7))
QUI
expect_failure package-index INDEX_BOUNDS 4 1 101 --link "$TMP/apply.o"

# The same failure in a file the package reaches through its own quoted
# import: still the user's statement.
cat > "$TMP/package-internal.qui" <<'QUI'
import loc = loc_pkg

int[] data = [1, 2, 3]
int r = loc.via_internal(data, 9)
QUI
expect_failure package-internal INDEX_BOUNDS 4 1 101 --link "$TMP/apply.o"

# A package that calls a user `fn` value and then fails: the user's call
# into the package, not the callback's last statement.
cat > "$TMP/package-callback.qui" <<'QUI'
import loc = loc_pkg

int one(int x)
    int y = x + 1
    return y

int[] data = [1, 2, 3]
print(loc.apply_then_fail(one, data))
QUI
expect_failure package-callback INDEX_BOUNDS 8 1 101 --link "$TMP/apply.o"

# A package that passes a user `fn` value to a C function, which calls it,
# and then fails: the same.
cat > "$TMP/package-native-callback.qui" <<'QUI'
import loc = loc_pkg

int64 one(int64 x)
    int64 y = x + 1
    return y

int[] data = [1, 2, 3]
print(loc.apply_native_then_fail(one, data))
QUI
expect_failure package-native-callback INDEX_BOUNDS 8 1 101 --link "$TMP/apply.o"

# A package failure after a user call in the same statement: the statement,
# not the user function's last statement.
cat > "$TMP/nested-user-call.qui" <<'QUI'
import loc = loc_pkg

int scale(int x)
    int y = x * 3
    return y

int[] data = [1, 2, 3]
int r = loc.pick(data, scale(3))
QUI
expect_failure nested-user-call INDEX_BOUNDS 8 1 101 --link "$TMP/apply.o"

# A package call inside a user function called from the failing statement:
# the innermost user statement, the callee's.
cat > "$TMP/package-in-callee.qui" <<'QUI'
import loc = loc_pkg

int inner(int[] v)
    int z = loc.pick(v, 9)
    return z

int[] data = [1, 2, 3]
print(inner(data))
QUI
expect_failure package-in-callee INDEX_BOUNDS 4 5 101 --link "$TMP/apply.o"

# A user statement that calls a C function with a user callback and then a
# failing package function: the statement.
cat > "$TMP/extern-callback-then-package.qui" <<'QUI'
import loc = loc_pkg

extern int64 c_apply(fn<int64>(int64) callback, int64 value) = "location_test_apply_user"

int64 twice(int64 x)
    int64 y = x * 2
    return y

int[] data = [1, 2, 3]
int r = loc.pick(data, int(c_apply(twice, 4)))
QUI
expect_failure extern-callback-then-package INDEX_BOUNDS 10 1 101 --link "$TMP/apply.o"

# The allocation helper fails without a location of its own after a user
# call returned in the same statement: the statement.
cat > "$TMP/allocation-after-call.qui" <<'QUI'
int count(int x)
    int n = x * 1537228672809129301
    return n

int[] xs = array(nat(count(3)))
print(len(xs))
QUI
expect_failure allocation-after-call INTEGER_OVERFLOW 5 1 101

# The initialization check of a load through a reference names its own
# statement, whatever user statement ran last.
cat > "$TMP/uninitialized-after-call.qui" <<'QUI'
int f(int x)
    int y = x + 1
    return y

int[] values = array(2)
int &r = &values[1]
int z = f(1) + r
QUI
expect_failure uninitialized-after-call UNINITIALIZED 7 1 101

# A test assertion after a user call in the same statement: the assertion's
# statement.
cat > "$TMP/assertion-after-call.qui" <<'QUI'
int add(int a, int b)
    int s = a + b
    return s

test.check(add(1, 2) == 4)
QUI
expect_failure assertion-after-call TEST 5 1 1


# --- The report's text: the header names the file as it was given to the
# compiler; the snippet appears only while the compiled file is present and
# identical (its size and SHA-256 recorded at compile time).
cat > "$TMP/snippet.qui" <<'QUI'
int[] values = [1, 2, 3]
print(values[3])
QUI
build snippet || fail "snippet: build failed"
# An element index failure is reported at the index operand.
snippet_fields=(code=INDEX_BOUNDS "file=$TMP/snippet.qui" line=2 column=14
                "message=index 3 out of bounds for length 3")
check_run snippet 101 "${snippet_fields[@]}" snippet=yes "source=print(values[3])" caret=14
# The executable embeds no source text and finds the file by its absolute
# path: run from another directory, it still shows the snippet.
mkdir -p "$TMP/elsewhere"
rc=0
(cd "$TMP/elsewhere" && "$TMP/snippet" > /dev/null 2> "$TMP/snippet-elsewhere.err") || rc=$?
if [[ "$rc" -eq 101 ]] && python3 "$HELPER" "$TMP/snippet-elsewhere.err" snippet=yes; then
    echo "ok   snippet-elsewhere"
else
    fail "snippet-elsewhere"
fi

# no_snippet NAME CHANGE: builds a copy of snippet.qui as NAME, applies the
# shell command CHANGE to $TMP/NAME.qui after the build, and expects the
# report without the snippet: the message right after the header.
no_snippet() {
    local name=$1 change=$2
    cp "$TMP/snippet.qui" "$TMP/$name.qui"
    build "$name" || { fail "$name: build failed"; return; }
    eval "$change"
    check_run "$name" 101 code=INDEX_BOUNDS "file=$TMP/$name.qui" line=2 column=14 snippet=no \
        "message=index 3 out of bounds for length 3"
}
no_snippet deleted 'rm "$TMP/deleted.qui"'
# The same size and line count, other bytes: a different digest.
no_snippet edited-same-size 'printf "int[] values = [4, 5, 6]\nprint(values[3])\n" > "$TMP/edited-same-size.qui"'
no_snippet edited-other-size 'printf "// changed\n" >> "$TMP/edited-other-size.qui"'
no_snippet directory 'rm "$TMP/directory.qui"; mkdir "$TMP/directory.qui"'
no_snippet fifo 'rm "$TMP/fifo.qui"; mkfifo "$TMP/fifo.qui"'
if [[ "$(id -u)" -ne 0 ]]; then
    no_snippet unreadable 'chmod 000 "$TMP/unreadable.qui"'
fi

# A file of several MiB failing on its last line: the snippet is shown.
python3 - "$TMP/large.qui" <<'PY'
import sys
with open(sys.argv[1], "w") as out:
    out.write("int[] values = [1, 2, 3]\n")
    for i in range(40000):
        out.write("// " + "x" * 76 + "\n")
    out.write("print(values[3])\n")
PY
build large || fail "large: build failed"
check_run large 101 code=INDEX_BOUNDS line=40002 column=14 snippet=yes "source=print(values[3])"

# Gutters of one, two and four digits; indentation kept.
python3 - "$TMP" <<'PY'
import sys
root = sys.argv[1]
with open(f"{root}/gutter-two.qui", "w") as out:
    out.write("int[] data = [1, 2, 3]\n" + "\n" * 10 + "print(data[3])\n")
with open(f"{root}/gutter-four.qui", "w") as out:
    out.write("int[] data = [1, 2, 3]\n" + "\n" * 1000 + "print(data[3])\n")
with open(f"{root}/indented.qui", "w") as out:
    out.write("int pick(int[] values, int i)\n    if i >= 0\n        return values[i]\n    return 0\n\nint[] data = [1, 2, 3]\nprint(pick(data, 5))\n")
PY
build gutter-two || fail "gutter-two: build failed"
check_run gutter-two 101 line=12 column=12 snippet=yes "source=print(data[3])" caret=12
build gutter-four || fail "gutter-four: build failed"
check_run gutter-four 101 line=1002 column=12 snippet=yes "source=print(data[3])" caret=12
build indented || fail "indented: build failed"
check_run indented 101 line=3 column=23 snippet=yes "source=        return values[i]" caret=23

# The caret stands under the column by display width: a tab inside a string
# literal, Japanese text and a combining mark before the column, a CRLF file.
caret_row() {
    local name=$1 literal=$2
    printf 'void pair(string text, int value)\n    print(value)\n\nint[] values = [1, 2, 3]\npair("%s", values[3])\n' \
        "$literal" > "$TMP/$name.qui"
    build "$name" || { fail "$name: build failed"; return; }
    local column
    column=$(python3 -c 'import sys; print(len(("pair(\"" + sys.argv[1] + "\", values[").encode()) + 1)' "$literal")
    check_run "$name" 101 line=5 "column=$column" snippet=yes "caret=$column"
}
caret_row caret-tab "$(printf 'a\tb')"
caret_row caret-japanese "日本語"
caret_row caret-combining "$(printf 'e\xcc\x81')"
printf 'int[] values = [1, 2, 3]\r\nprint(values[3])\r\n' > "$TMP/crlf.qui"
build crlf || fail "crlf: build failed"
check_run crlf 101 line=2 column=14 snippet=yes "source=print(values[3])" caret=14

# A line longer than the line buffer is written in full by a second read.
python3 - "$TMP/long-line.qui" <<'PY'
import sys
with open(sys.argv[1], "w") as out:
    out.write('void pair(string text, int value)\n    print(value)\n\nint[] values = [1, 2, 3]\n')
    out.write('pair("' + "y" * 70000 + '", values[3])\n')
PY
build long-line || fail "long-line: build failed"
check_run long-line 101 line=5 column=70017 snippet=yes caret=70017 "source=/^pair\\(\"y{70000}\", values\\[3\\]\\)$/"

# --- Files: a failure in a local import names its display path, the root's
# directory as the root was given joined with the import's relative path,
# and its own line; the root's spelling is kept as given.
mkdir -p "$TMP/files/lib"
cat > "$TMP/files/lib/util.qui" <<'QUI'
int pick(int[] values, int i)
    return values[i]
QUI
cat > "$TMP/files/main.qui" <<'QUI'
import util = "./lib/util.qui"

int[] data = [1, 2, 3]
print(util.pick(data, 9))
QUI
(cd "$TMP" && "$QUIDRA" build files/main.qui -o files/main > /dev/null 2>&1) || fail "files: build failed"
check_run files/main 101 code=INDEX_BOUNDS file=files/lib/util.qui line=2 column=19 snippet=yes \
    "source=    return values[i]"
# The import's failure immediates carry its index (1) in their upper 32 bits.
(cd "$TMP" && "$QUIDRA" llvm files/main.qui > files/main.ll) || fail "files: llvm failed"
if grep -q 'i64 4294967298, i64 19)' "$TMP/files/main.ll"; then echo "ok   files-immediates"; else fail "files-immediates"; fi
for spelling in ./snippet.qui snippet.qui "$TMP/snippet.qui"; do
    rc=0
    (cd "$TMP" && "$QUIDRA" run "$spelling" > /dev/null 2> "$TMP/spelling.err") || rc=$?
    if [[ "$rc" -eq 101 ]] && python3 "$HELPER" "$TMP/spelling.err" "file=$spelling" line=2 column=14; then
        echo "ok   spelling $spelling"
    else
        fail "spelling $spelling"; cat "$TMP/spelling.err"
    fi
done
mkdir -p "$TMP/dir"
cp "$TMP/snippet.qui" "$TMP/dir/prog.qui"
rc=0
(cd "$TMP" && "$QUIDRA" run dir/prog.qui > /dev/null 2> "$TMP/dir-spelling.err") || rc=$?
if [[ "$rc" -eq 101 ]] && python3 "$HELPER" "$TMP/dir-spelling.err" file=dir/prog.qui snippet=yes; then
    echo "ok   spelling dir/prog.qui"
else
    fail "spelling dir/prog.qui"
fi
# The REPL names <repl> and shows no snippet.
rc=0
printf 'int[] values = [1, 2, 3]\nprint(values[3])\n' | (cd "$TMP" && "$QUIDRA" repl) > "$TMP/repl.out" 2>&1 || rc=$?
if python3 "$HELPER" "$TMP/repl.out" code=INDEX_BOUNDS file='<repl>' snippet=no; then
    echo "ok   repl"
else
    fail "repl"
fi

# --- Uncoded runtime-library failures: the header without a code, at the
# user's statement, then the unchanged message.
cat > "$TMP/uncoded-split.qui" <<'QUI'
string empty_of(string text)
    return text.slice(0, 0)

string line = "a,b"
string[] parts = line.split(empty_of(line))
QUI
build uncoded-split || fail "uncoded-split: build failed"
check_run uncoded-split 101 code= line=5 column=1 snippet=yes "message=string split separator cannot be empty"
cat > "$TMP/uncoded-repeat.qui" <<'QUI'
string fill = "ab"
string text = string.repeat(fill, 3)
QUI
build uncoded-repeat || fail "uncoded-repeat: build failed"
check_run uncoded-repeat 101 code= line=2 column=1 "message=string fill must contain exactly one Unicode code point"
cat > "$TMP/uncoded-bin-fill.qui" <<'QUI'
int bit_of(string text)
    return int(len(text))

bin data = bin.fill(8, bit_of("ab"))
QUI
build uncoded-bin-fill || fail "uncoded-bin-fill: build failed"
check_run uncoded-bin-fill 101 code= line=4 column=1 "message=bin fill must be 0 or 1"

# --- Two task.all tasks failing at once: one complete report, status 101,
# and the output written before the failures.
cat > "$TMP/two-tasks.qui" <<'QUI'
void first()
    int[] data = [1, 2, 3]
    print(data[5])

void second()
    int[] data = [1, 2, 3]
    print(data[7])

print("before" + NL)
task.all([first, second])
QUI
build two-tasks || fail "two-tasks: build failed"
check_run two-tasks 101 code=INDEX_BOUNDS "file=$TMP/two-tasks.qui"
if [[ "$(cat "$TMP/two-tasks.out")" == before && "$(grep -c '^Quidra runtime error' "$TMP/two-tasks.err")" -eq 1 ]]; then
    echo "ok   two-tasks-one-report"
else
    fail "two-tasks-one-report"; cat "$TMP/two-tasks.err"
fi

# --- A failing test.check: the assertion's statement and the snippet, no
# message; status 1.
cat > "$TMP/assertion.qui" <<'QUI'
int answer = 41
test.check(answer == 42)
QUI
build assertion || fail "assertion: build failed"
check_run assertion 1 kind=test_assertion "file=$TMP/assertion.qui" line=2 column=1 snippet=yes \
    "source=test.check(answer == 42)" message=

if [[ "$failures" -ne 0 ]]; then
    echo "runtime_location_tests: $failures failed"
    exit 1
fi
echo "runtime_location_tests: passed"
