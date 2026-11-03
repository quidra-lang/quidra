#!/usr/bin/env bash
# The trace of a runtime error report (docs/spec/diagnostics.md, "Error
# values"): the hops an error propagated through, inner to outer, "unhandled
# at" for a stored error stopped elsewhere, and the caused-by lines of a
# wrapped error with their own hops; elision; the JSON report's hops and
# causes. Each program runs as main.qui from its own directory, so the
# report shows the snippet and names main.qui.
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'chmod -R u+rw "$TMP" 2>/dev/null || true; rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# program NAME < PROGRAM: $TMP/NAME/main.qui.
program() {
    mkdir -p "$TMP/$1"
    cat > "$TMP/$1/main.qui"
}

# expect NAME [STATUS [BUILD OPTIONS...]] < TEXT: builds $TMP/NAME/main.qui
# (with the build options), runs it from its directory and checks that it
# stops with STATUS (101 by default) and that its standard error is exactly
# TEXT.
expect() {
    local name=$1
    local status=${2:-101}
    shift; shift || true
    cat > "$TMP/$name/expected.txt"
    if ! (cd "$TMP/$name" && "$QUIDRA" build main.qui -o prog "$@" > build.log 2>&1); then
        fail "$name: build failed"; cat "$TMP/$name/build.log"; return 0
    fi
    local rc=0
    (cd "$TMP/$name" && ./prog > out.txt 2> err.txt) || rc=$?
    if [[ "$rc" -ne "$status" ]]; then
        fail "$name: exit status $rc"; cat "$TMP/$name/err.txt"; return 0
    fi
    if ! cmp -s "$TMP/$name/expected.txt" "$TMP/$name/err.txt"; then
        fail "$name: report"
        diff "$TMP/$name/expected.txt" "$TMP/$name/err.txt" || true
        return 0
    fi
    echo "ok   $name"
}

# --- User try propagation through four functions: one hop per try of a
# call, at the call (never at try), then the top-level fail-fast's hop.
program propagation <<'QUI'
int | error parse_port(string text)
    if text == ""
        return error("port is empty", code = "CONFIG_PORT")
    return try int.parse(text)

int | error read_port(string path)
    string text := try string.from_utf8(try file.read_bin(path))
    return try parse_port(text)

int | error load_settings(string path)
    int port := try read_port(path)
    return port

int | error start_server(string path)
    int port := try load_settings(path)
    return port + 1

int port := start_server("port.txt")
print(port)
print(NL)
QUI
: > "$TMP/propagation/port.txt"
expect propagation <<'TXT'
Quidra runtime error[CONFIG_PORT] at main.qui:3:16

3 |         return error("port is empty", code = "CONFIG_PORT")
  |                ^
  | port is empty
  |
  | propagated through parse_port() at main.qui:8:16
  | propagated through read_port() at main.qui:11:21
  | propagated through load_settings() at main.qui:15:21
  | propagated through start_server() at main.qui:18:13
TXT

# --- A four-deep wrap chain: each cause with the hop where it left its
# function, indented under its caused-by line.
program wrap-chain <<'QUI'
int | error read_number(string text)
    return try int.parse(text)

int | error read_port(string text)
    match read_number(text)
        int port
            return port
        error problem
            return error("invalid port", cause = problem, code = "CONFIG_PORT")

int | error load_config(string text)
    match read_port(text)
        int port
            return port
        error problem
            return error("cannot load configuration", cause = problem, code = "CONFIG_LOAD")

int | error start(string text)
    match load_config(text)
        int port
            return port
        error problem
            return error("server did not start", cause = problem, code = "SERVER_START")

int port := start("80x")
print(port)
print(NL)
QUI
expect wrap-chain <<'TXT'
Quidra runtime error[SERVER_START] at main.qui:23:20

23 |             return error("server did not start", cause = problem, code = "SERVER_START")
   |                    ^
   | server did not start
   |
   | propagated through start() at main.qui:25:13
   | caused by: [CONFIG_LOAD] cannot load configuration (at main.qui:16:20)
   |     propagated through load_config() at main.qui:19:11
   | caused by: [CONFIG_PORT] invalid port (at main.qui:9:20)
   |     propagated through read_port() at main.qui:12:11
   | caused by: [NUMERIC_PARSE] cannot parse numeric input as int (at main.qui:2:16)
   |     propagated through read_number() at main.qui:5:11
TXT

# --- The mixed case: the cause propagated before it was wrapped, the wrapper
# after. (A file without read permission: skipped when running as root.)
program mixed <<'QUI'
string | error read_file(string path)
    return try string.from_utf8(try file.read_bin(path))

string | error read_dataset(string path)
    string text := try read_file(path)
    return text

string | error load_dataset(string path)
    match read_dataset(path)
        string text
            return text
        error problem
            return error("dataset loading failed", cause = problem, code = "DATASET_LOAD")

int | error train(string path)
    string data := try load_dataset(path)
    return int(len(data))

int steps := train("data.csv")
print(steps)
print(NL)
QUI
if [[ "$(id -u)" -ne 0 ]]; then
    printf 'x' > "$TMP/mixed/data.csv"
    chmod 000 "$TMP/mixed/data.csv"
    expect mixed <<'TXT'
Quidra runtime error[DATASET_LOAD] at main.qui:13:20

13 |             return error("dataset loading failed", cause = problem, code = "DATASET_LOAD")
   |                    ^
   | dataset loading failed
   |
   | propagated through load_dataset() at main.qui:16:24
   | propagated through train() at main.qui:19:14
   | caused by: [FILE_READ] cannot read "data.csv": permission denied (at main.qui:2:37)
   |     propagated through read_file() at main.qui:5:24
   |     propagated through read_dataset() at main.qui:9:11
TXT
    # The JSON report carries every hop with how the error left it, and the
    # causes with their details and hops.
    if ! (cd "$TMP/mixed" && QUIDRA_ERROR_FORMAT=json ./prog > /dev/null 2> json.txt; \
          python3 - json.txt <<'PY'
import json, sys
report = json.loads(open(sys.argv[1]).read().splitlines()[0])
assert report["code"] == "DATASET_LOAD", report
assert report["details"] == {"text": "dataset loading failed"}, report
assert [(h["function"], h["line"], h["column"], h["count"], h["via"]) for h in report["hops"]] == [
    ("load_dataset", 16, 24, 1, "try"), ("train", 19, 14, 1, "fail_fast")], report["hops"]
assert report["unhandled_at"] is None, report
cause = report["causes"][0]
assert len(report["causes"]) == 1 and cause["code"] == "FILE_READ", report["causes"]
assert cause["details"] == {"path": "data.csv", "reason": "PERMISSION_DENIED"}, cause
assert cause["location"]["line"] == 2 and cause["location"]["column"] == 37, cause
assert [(h["function"], h["via"]) for h in cause["hops"]] == [
    ("read_file", "try"), ("read_dataset", "match")], cause["hops"]
PY
    ); then
        fail "mixed-json"; cat "$TMP/mixed/json.txt"
    else
        echo "ok   mixed-json"
    fi
fi

# --- A stored error that fails fast away from its origin: no hop names the
# stop, so it is printed.
program unhandled <<'QUI'
int | error check(int value)
    if value < 0
        return error("negative value", code = "VALUE_RANGE")
    return value

int total(int[] values)
    int | error last := check(values[0])
    int checked := last
    return checked + 1

int[] data := [-1]
print(total(data))
print(NL)
QUI
expect unhandled <<'TXT'
Quidra runtime error[VALUE_RANGE] at main.qui:3:16

3 |         return error("negative value", code = "VALUE_RANGE")
  |                ^
  | negative value
  |
  | unhandled at main.qui:8:20
  | propagated through total() at main.qui:12:7
TXT

# --- A recursion propagating through one site: one line, "(N times)".
program recursion <<'QUI'
int | error down(int n)
    if n == 0
        return error("bottom", code = "BOTTOM")
    int value := try down(n - 1)
    return value

int r := down(30)
QUI
expect recursion <<'TXT'
Quidra runtime error[BOTTOM] at main.qui:3:16

3 |         return error("bottom", code = "BOTTOM")
  |                ^
  | bottom
  |
  | propagated through down() at main.qui:4:22 (30 times)
  | propagated through down() at main.qui:7:10
TXT

# --- More than 20 lines: the first 10, "… N more …" and the last 10.
program elided <<'QUI'
int | error odd(int n);

int | error even(int n)
    if n == 0
        return error("end", code = "END")
    int value := try odd(n - 1)
    return value

int | error odd(int n)
    int value := try even(n - 1)
    return value

int r := even(30)
QUI
{
    printf '%s\n' 'Quidra runtime error[END] at main.qui:5:16' '' \
        '5 |         return error("end", code = "END")' '  |                ^' '  | end' '  |'
    for i in 1 2 3 4 5; do
        printf '%s\n' '  | propagated through even() at main.qui:10:22' \
            '  | propagated through odd() at main.qui:6:22'
    done
    printf '%s\n' '  | … 11 more …'
    for i in 1 2 3 4 5; do
        printf '%s\n' '  | propagated through odd() at main.qui:6:22' \
            '  | propagated through even() at main.qui:10:22'
    done | head -n 9
    printf '%s\n' '  | propagated through even() at main.qui:13:10'
} | expect elided

# --- Copy on write: a hop on an error that a stored value also holds goes to
# a copy; the stored value keeps no hop.
program copy-on-write <<'QUI'
int | error make()
    return error("boom", code = "BOOM")

int | error again(error problem)
    return problem

int | error relay(error problem)
    int value := try again(problem)
    return value

error[] kept := []
match make()
    int value
        print(value)
    error problem
        kept = [problem]
match relay(kept[0])
    int value
        print(value)
    error relayed
        print("{relayed == kept[0]}{NL}")
int | error stored := kept[0]
int stopped := stored
QUI
expect copy-on-write <<'TXT'
Quidra runtime error[BOOM] at main.qui:2:12

2 |     return error("boom", code = "BOOM")
  |            ^
  | boom
  |
  | propagated through make() at main.qui:12:7
  | unhandled at main.qui:23:16
TXT
if [[ "$(cat "$TMP/copy-on-write/out.txt" 2>/dev/null)" != "true" ]]; then
    fail "copy-on-write: equality across hops"; cat "$TMP/copy-on-write/out.txt"
fi

# ---------------------------------------------------------------------------
# The walked path of plain code (docs/spec/diagnostics.md, "The walked path"):
# traps and fail-fast stops print the user calls they are inside, inner to
# outer, whether or not LLVM inlined them.

# --- A trap, one function deep.
program trap-one-deep <<'QUI'
int64 scale(int64 x)
    return x * 1000000000000

int64 big := int64.parse("10000000")
print(scale(big))
print(NL)
QUI
expect trap-one-deep <<'TXT'
Quidra runtime error[INTEGER_OVERFLOW] at main.qui:2:12

2 |     return x * 1000000000000
  |            ^
  | integer overflow
  |
  | propagated through scale() at main.qui:5:7
TXT

# --- Plain code, four deep.
program plain-four-deep <<'QUI'
int pick(int[] values, int i)
    return values[i]

int sum_first(int[] values, int n)
    int first := pick(values, 0)
    int last := pick(values, n)
    return first + last

int score(int[] values)
    return sum_first(values, 4) * 2

int report(int[] values)
    return score(values) + 1

int[] data := [1, 2, 3, 4]
int total := report(data)
print(total)
print(NL)
QUI
plain_four_deep='Quidra runtime error[INDEX_BOUNDS] at main.qui:2:19

2 |     return values[i]
  |                   ^
  | index 4 out of bounds for length 4
  |
  | propagated through pick() at main.qui:6:17
  | propagated through sum_first() at main.qui:10:12
  | propagated through score() at main.qui:13:12
  | propagated through report() at main.qui:16:14'
expect plain-four-deep <<< "$plain_four_deep"

# The JSON report's path: every frame with its function, position, count
# and how it was reached.
if ! (cd "$TMP/plain-four-deep" && QUIDRA_ERROR_FORMAT=json ./prog > /dev/null 2> json.txt; \
      python3 - json.txt <<'PY'
import json, sys
report = json.loads(open(sys.argv[1]).read().splitlines()[0])
assert report["path_complete"] is True, report
assert [(p["function"], p["line"], p["column"], p["count"]) for p in report["path"]] == [
    ("pick", 6, 17, 1), ("sum_first", 10, 12, 1), ("score", 13, 12, 1), ("report", 16, 14, 1)], report["path"]
assert all(p["via"] in ("call", "inline") and p["file"] == "main.qui" for p in report["path"]), report["path"]
assert report["details"] == {"index": 4, "length": 4}, report
PY
); then
    fail "plain-four-deep-json"; cat "$TMP/plain-four-deep/json.txt"
else
    echo "ok   plain-four-deep-json"
fi

# A stripped executable keeps its table (data, not symbols).
cp "$TMP/plain-four-deep/prog" "$TMP/plain-four-deep/stripped"
if strip "$TMP/plain-four-deep/stripped" 2>/dev/null; then
    rc=0
    (cd "$TMP/plain-four-deep" && ./stripped > /dev/null 2> stripped.txt) || rc=$?
    if [[ "$rc" -ne 101 ]] || [[ "$(cat "$TMP/plain-four-deep/stripped.txt")" != "$plain_four_deep" ]]; then
        fail "plain-four-deep-stripped"; cat "$TMP/plain-four-deep/stripped.txt"
    else
        echo "ok   plain-four-deep-stripped"
    fi
fi

# Release executables are reproducible: two builds of the same inputs into
# different directories are byte-identical, and a Mach-O executable has no
# debug map naming the build's object.
mkdir -p "$TMP/plain-four-deep/again"
if (cd "$TMP/plain-four-deep" && "$QUIDRA" build main.qui -o again/prog > again/build.log 2>&1) &&
    cmp -s "$TMP/plain-four-deep/prog" "$TMP/plain-four-deep/again/prog"; then
    echo "ok   plain-four-deep-reproducible"
else
    fail "plain-four-deep-reproducible"
fi
if [[ "$(uname)" == Darwin ]] && nm -ap "$TMP/plain-four-deep/prog" | grep -q ' OSO '; then
    fail "plain-four-deep: a debug map in the release executable"
fi

# --debug builds name the same sites.
mkdir -p "$TMP/plain-four-deep-debug"
cp "$TMP/plain-four-deep/main.qui" "$TMP/plain-four-deep-debug/main.qui"
expect plain-four-deep-debug 101 --debug <<< "$plain_four_deep"

# --- A fail-fast library error: the path is walked from the function that
# stopped.
program library-fail-fast <<'QUI'
string read_config(string path)
    string text := string.from_utf8(file.read_bin(path))
    return text

int config_length(string path)
    string text := read_config(path)
    return int(len(text))

int n := config_length("settings.toml")
print(n)
print(NL)
QUI
expect library-fail-fast <<'TXT'
Quidra runtime error[FILE_READ] at main.qui:2:37

2 |     string text := string.from_utf8(file.read_bin(path))
  |                                     ^
  | cannot read "settings.toml": file not found
  |
  | propagated through read_config() at main.qui:6:20
  | propagated through config_length() at main.qui:9:10
TXT

# --- Without a snippet: the same, after main.qui was deleted.
program no-snippet <<'QUI'
string read_config(string path)
    string text := string.from_utf8(file.read_bin(path))
    return text

int config_length(string path)
    string text := read_config(path)
    return int(len(text))

int n := config_length("settings.toml")
print(n)
print(NL)
QUI
if (cd "$TMP/no-snippet" && "$QUIDRA" build main.qui -o prog > build.log 2>&1); then
    rm "$TMP/no-snippet/main.qui"
    rc=0
    (cd "$TMP/no-snippet" && ./prog > /dev/null 2> err.txt) || rc=$?
    if [[ "$rc" -ne 101 ]] || [[ "$(cat "$TMP/no-snippet/err.txt")" != 'Quidra runtime error[FILE_READ] at main.qui:2:37
| cannot read "settings.toml": file not found
|
| propagated through read_config() at main.qui:6:20
| propagated through config_length() at main.qui:9:10' ]]; then
        fail "no-snippet"; cat "$TMP/no-snippet/err.txt"
    else
        echo "ok   no-snippet"
    fi
else
    fail "no-snippet: build failed"
fi

# --- Deep recursion through one site: the identical lines collapse.
program depth-one-site <<'QUI'
int depth(int n)
    return depth(n + 1) + 1

print(depth(0))
print(NL)
QUI
expect depth-one-site <<'TXT'
Quidra runtime error[CALL_DEPTH_LIMIT] at main.qui:2:12

2 |     return depth(n + 1) + 1
  |            ^
  | maximum recursion depth exceeded (limit 4096)
  |
  | propagated through depth() at main.qui:2:12 (4095 times)
  | propagated through depth() at main.qui:4:7
TXT

# --- Deep mutual recursion: the prologue's guard fails, the report names
# the call; 4096 lines elide to the first and last 10.
program depth-alternating <<'QUI'
bool is_odd(int n);

bool is_even(int n)
    if n == 0
        return true
    return is_odd(n - 1)

bool is_odd(int n)
    if n == 0
        return false
    return is_even(n - 1)

print(is_even(100000))
print(NL)
QUI
{
    printf '%s\n' 'Quidra runtime error[CALL_DEPTH_LIMIT] at main.qui:11:12' '' \
        '11 |     return is_even(n - 1)' '   |            ^' \
        '   | maximum recursion depth exceeded (limit 4096)' '   |'
    for i in 1 2 3 4 5; do
        printf '%s\n' '   | propagated through is_odd() at main.qui:6:12' \
            '   | propagated through is_even() at main.qui:11:12'
    done
    printf '%s\n' '   | … 4076 more …'
    for i in 1 2 3 4; do
        printf '%s\n' '   | propagated through is_odd() at main.qui:6:12' \
            '   | propagated through is_even() at main.qui:11:12'
    done
    printf '%s\n' '   | propagated through is_odd() at main.qui:6:12' \
        '   | propagated through is_even() at main.qui:13:7'
} | expect depth-alternating

# --- An uncoded runtime-library failure.
program uncoded <<'QUI'
string[] fields(string line, string separator)
    return line.split(separator)

string separator := ""
string[] parts := fields("a,b", separator)
print(len(parts))
print(NL)
QUI
expect uncoded <<'TXT'
Quidra runtime error at main.qui:2:5

2 |     return line.split(separator)
  |     ^
  | string split separator cannot be empty
  |
  | propagated through fields() at main.qui:5:19
TXT

# --- A test assertion (status 1): no message, then the path.
program assertion <<'QUI'
bool all_positive(int[] values)
    test.check(len(values) > 3)
    return true

bool ok := all_positive([1, 2])
QUI
expect assertion 1 <<'TXT'
Quidra test assertion failed at main.qui:2:5

2 |     test.check(len(values) > 3)
  |     ^
  |
  | propagated through all_positive() at main.qui:5:12
TXT

# --- A failing task body: its path ends with the parallel call.
program task-body <<'QUI'
int pick(int[] values, nat i)
    return values[i]

void fails()
    int[] values := [1, 2, 3]
    print(pick(values, len(values)))

void waits()
    int unused := 0

parallel
    fails()
    waits()
QUI
expect task-body <<'TXT'
Quidra runtime error[INDEX_BOUNDS] at main.qui:2:19

2 |     return values[i]
  |                   ^
  | index 3 out of bounds for length 3
  |
  | propagated through pick() at main.qui:6:11
  | propagated through fails() at main.qui:12:5
  | propagated through <node body>() at main.qui:12:5
TXT

# --- Calls LLVM merges: the two calls of heavy() in the branches of an if
# become one machine call; the line names both candidate sites.
program merged <<'QUI'
int heavy(int[] values, int at)
    int total := 0
    for v := values[]
        total = total + v * v
    return total + values[at]

int choose(int[] values, bool first, int a, int b)
    int r := 0
    if first
        r = heavy(values, a)
    else
        r = heavy(values, b)
    return r + 1

int[] values := [1, 2, 3]
int one := int.parse("1")
print(choose(values, one == 1, 7, 8))
print(NL)
QUI
(cd "$TMP/merged" && "$QUIDRA" build main.qui -o prog > build.log 2>&1 && ./prog > /dev/null 2> err.txt) || true
if grep -q 'propagated through heavy() at main.qui:10:13 or main.qui:12:13$' "$TMP/merged/err.txt" ||
    grep -q 'propagated through heavy() at main.qui:10:13$' "$TMP/merged/err.txt"; then
    echo "ok   merged"
else
    fail "merged"; cat "$TMP/merged/err.txt"
    if [[ "$(uname -s)" == Linux ]] && command -v objdump >/dev/null 2>&1; then
        objdump -s -j quidra_calls -j quidra_sites "$TMP/merged/prog" || true
    fi
fi

# The source-level path stays complete when an inlined shape wrapper loses
# its DWARF identity at a merged call. JSON must retain the same candidate
# sites as text, without exposing the compiler's wrapper/body split.
if ! python3 - "$TMP/merged" <<'PY'
import json, os, subprocess, sys
from pathlib import Path

root = Path(sys.argv[1])
ran = subprocess.run(["./prog"], cwd=root, text=True, capture_output=True,
                     env=dict(os.environ, QUIDRA_ERROR_FORMAT="json"))
(root / "json.txt").write_text(ran.stderr)
assert ran.returncode == 101, (ran.returncode, ran.stdout, ran.stderr)
report = json.loads(ran.stderr.splitlines()[0])
assert report["path_complete"] is True, report
path = report["path"]
assert [p["function"] for p in path] == ["heavy", "choose"], path
heavy, choose = path
positions = [(p["file"], p["line"], p["column"]) for p in heavy.get("sites", [heavy])]
assert positions in ([('main.qui', 10, 13), ('main.qui', 12, 13)], [('main.qui', 10, 13)]), heavy
assert (choose["file"], choose["line"], choose["column"]) == ("main.qui", 17, 7), choose
assert all(p["count"] == 1 and p["via"] in ("call", "inline") for p in path), path
PY
then
    fail "merged-json"; cat "$TMP/merged/json.txt"
else
    echo "ok   merged-json"
fi

# --- An error that leaves a call through an fn value names the function
# actually called.
program indirect-hop <<'QUI'
int | error parse_value(string text)
    return try int.parse(text)

int | error apply(func<int | error>(string) op, string text)
    int value := try op(text)
    return value

int r := apply(parse_value, "x")
QUI
expect indirect-hop <<'TXT'
Quidra runtime error[NUMERIC_PARSE] at main.qui:2:16

2 |     return try int.parse(text)
  |                ^
  | cannot parse numeric input as int
  |
  | propagated through parse_value() at main.qui:5:22
  | propagated through apply() at main.qui:8:10
TXT

if [[ "$failures" -ne 0 ]]; then
    echo "$failures runtime trace row(s) failed"
    exit 1
fi
echo "runtime trace tests passed"
