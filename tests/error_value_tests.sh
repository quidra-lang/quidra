#!/usr/bin/env bash
# Error values (docs/spec/diagnostics.md, "Error values"): each Core code from
# its source, read back through its message and through a fail-fast stop,
# whose report names the error's own code at its origin; user errors; errors
# made in package code; the JSON report's details.
set -euo pipefail

QUIDRA="$1"
HELPER="$(cd "$(dirname "$0")" && pwd)/runtime_report.py"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
build_extra_arguments=()
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# build NAME < PROGRAM: $TMP/NAME.qui built to $TMP/NAME (packages from
# $TMP/packages).
build() {
    local name=$1
    cat > "$TMP/$name.qui"
    if ! QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" "${build_extra_arguments[@]}" \
            > "$TMP/$name.build" 2>&1; then
        fail "$name: build failed"; cat "$TMP/$name.build"; return 1
    fi
}

# stops NAME CODE LINE COLUMN MESSAGE [field=value ...] < PROGRAM: the program
# stops with status 101 after printing "before", and its report names CODE at
# LINE:COLUMN of the program with MESSAGE. Standard input is empty.
stops() {
    local name=$1 code=$2 line=$3 column=$4 message=$5
    shift 5
    build "$name" || return 0
    local rc=0
    (cd "$TMP" && "./$name" < /dev/null > "$TMP/$name.out" 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne 101 ]]; then
        fail "$name: exit status $rc, expected 101"; cat "$TMP/$name.err"; return 0
    fi
    if [[ "$(cat "$TMP/$name.out")" != before ]]; then
        fail "$name: standard output"; cat "$TMP/$name.out"; return 0
    fi
    if ! python3 "$HELPER" "$TMP/$name.err" "code=$code" "file=$TMP/$name.qui" "line=$line" \
            "column=$column" "message=$message" ${@+"$@"}; then
        fail "$name"; return 0
    fi
    echo "ok   $name"
}

# prints NAME EXPECTED < PROGRAM: the program exits with status 0 and prints
# EXPECTED.
prints() {
    local name=$1 expected=$2
    build "$name" || return 0
    local rc=0 out
    out=$(cd "$TMP" && "./$name" < /dev/null 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        fail "$name: exit status $rc"; cat "$TMP/$name.err"; return 0
    fi
    if [[ "$out" != "$expected" ]]; then
        fail "$name: printed"; printf '%s\n--- expected\n%s\n' "$out" "$expected"; return 0
    fi
    echo "ok   $name"
}

# details NAME JSON < PROGRAM: with QUIDRA_ERROR_FORMAT=json the report's
# code and details equal those of JSON (a JSON object with "code" and
# "details").
details() {
    local name=$1 expected=$2
    build "$name" || return 0
    local rc=0
    (cd "$TMP" && QUIDRA_ERROR_FORMAT=json "./$name" < /dev/null > "$TMP/$name.out" \
        2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne 101 ]]; then
        fail "$name: exit status $rc, expected 101"; cat "$TMP/$name.err"; return 0
    fi
    if ! python3 - "$TMP/$name.err" "$expected" <<'PY'
import json, sys
report = json.loads(open(sys.argv[1]).read().splitlines()[0])
expected = json.loads(sys.argv[2])
for key, value in expected.items():
    assert report[key] == value, (key, report[key], value)
PY
    then
        fail "$name"; cat "$TMP/$name.err"; return 0
    fi
    echo "ok   $name"
}

stops bare-error ERROR 2 1 "invalid input" <<'QUI'
print("before" + NL)
error("invalid input")
print("after" + NL)
QUI
stops bare-coded-error INPUT_INVALID 2 1 "invalid input" <<'QUI'
print("before" + NL)
error("invalid input", code = "INPUT_INVALID")
print("after" + NL)
QUI

# --- User errors: ERROR for an error without a code; the text is the message,
# the origin is the error(...) call.
stops user-literal ERROR 3 16 "negative value" <<'QUI'
int | error check(int value)
    if value < 0
        return error("negative value")
    return value
print("before" + NL)
int n := check(-1)
QUI
stops user-template ERROR 3 16 "bad value -1" <<'QUI'
int | error check(int value)
    if value < 0
        return error("bad value {value}")
    return value
print("before" + NL)
int n := check(-1)
QUI
details user-details '{"code": "ERROR", "details": {"text": "negative value"}}' <<'QUI'
int | error check(int value)
    if value < 0
        return error("negative value")
    return value
int n := check(-1)
QUI

# The message is what printing, interpolation and string() give; a runtime
# error kept in an array and read back stays intact.
prints user-message "$(printf 'negative value\nfailed: negative value\nnegative value\nnegative value -1 / negative value -1\n2')" <<'QUI'
int | error check(int value)
    if value < 0
        return error("negative value")
    return value
int | error described(int value)
    if value < 0
        return error("negative value {value}")
    return value
match check(-1)
    int n
        print(n)
    error problem
        print(problem)
        print(NL)
        print("failed: {problem}" + NL)
        print(string(problem) + NL)
match described(-1)
    int n
        print(n)
    error problem
        error[] kept := [problem, problem]
        print("{kept[0]} / {kept[1]}" + NL)
        print(kept.len())
QUI

# --- Core codes from their sources.
stops utf8-decode UTF8_DECODE 3 16 "invalid UTF-8 text" <<'QUI'
bin data := bin("11111111")
print("before" + NL)
string text := string.from_utf8(data)
QUI
stops json-parse JSON_PARSE 2 21 "expected JSON value at byte 1" <<'QUI'
print("before" + NL)
json.Value value := json.parse("[")
QUI
details json-parse-details '{"code": "JSON_PARSE", "details": {"problem": "expected JSON value", "offset": 1}}' <<'QUI'
json.Value value := json.parse("[")
QUI
stops input-end INPUT_END 3 1 "scan reached the end of input" <<'QUI'
int n := 0
print("before" + NL)
scan("{&n}")
QUI
stops conversion-int8 NUMERIC_CONVERSION 3 15 "numeric conversion out of range: value cannot be represented as int8" <<'QUI'
int value := 300
print("before" + NL)
int8 small := int8(value)
QUI
details conversion-details '{"code": "NUMERIC_CONVERSION", "details": {"type": "int8", "subject": "VALUE", "reason": "OUT_OF_RANGE"}}' <<'QUI'
int value := 300
int8 small := int8(value)
QUI

# One conversion site, two reasons: the site's record array is indexed by
# the reason. (A real64 that is not finite no longer exists to narrow, so the
# two reasons are an exact real's.)
prints conversion-reasons "$(printf 'numeric conversion failed: value is not an integer and cannot be represented as int64\nnumeric conversion out of range: value cannot be represented as int64')" <<'QUI'
int64 | error whole(real value)
    return int64(value)
real[] values := [0.5, 1.0e30]
for value := values[]
    match whole(value)
        int64 converted
            print(converted)
        error problem
            print(problem)
            print(NL)
QUI

# --- An error made in package code: its origin is the user statement that
# called into the package.
mkdir -p "$TMP/packages/err_pkg"
cat > "$TMP/packages/err_pkg/quidra.package" <<'MANIFEST'
name = err_pkg
version = 0.1.0
MANIFEST
cat > "$TMP/packages/err_pkg/main.qui" <<'QUI'
int | error checked(int value)
    if value < 0
        return error("package refused {value}", code = "ERR_PKG_REFUSED")
    return value

int8 | error narrowed(int value)
    return int8(value)
QUI
stops package-user-error ERR_PKG_REFUSED 4 1 "package refused -2" <<'QUI'
import pkg = err_pkg

print("before" + NL)
int n := pkg.checked(-2)
QUI
stops package-conversion NUMERIC_CONVERSION 4 1 "numeric conversion out of range: value cannot be represented as int8" <<'QUI'
import pkg = err_pkg

print("before" + NL)
int8 n := pkg.narrowed(1000)
QUI

# --- Parses: NUMERIC_PARSE names the type and, out of range, says so; the
# site's records are indexed by the reason.
prints parse-reasons "$(printf '%s\n' \
    'cannot parse numeric input as int64' \
    'cannot parse numeric input as int64: value out of range' \
    'cannot parse numeric input as int8: value out of range' \
    'cannot parse numeric input as nat8: value out of range' \
    'cannot parse numeric input as nat8' \
    'cannot parse numeric input as real32' \
    'cannot parse numeric input as real32: value out of range' \
    'cannot parse numeric input as int' \
    'cannot parse bin input')" <<'QUI'
string[] wide := ["12x", "99999999999999999999"]
for text := wide[]
    match int64(text)
        int64 value
            print(value)
        error problem
            print(problem)
            print(NL)
match int8("300")
    int8 value
        print(value)
    error problem
        print(problem)
        print(NL)
string[] small := ["-1", "abc"]
for text := small[]
    match nat8(text)
        nat8 value
            print(value)
        error problem
            print(problem)
            print(NL)
string[] reals := ["x", "1e60"]
for text := reals[]
    match real32(text)
        real32 value
            print(value)
        error problem
            print(problem)
            print(NL)
match int("1.5")
    int value
        print(value)
    error problem
        print(problem)
        print(NL)
string bits := "0102"
match bin(bits)
    bin value
        print(value)
    error problem
        print(problem)
QUI
stops parse-fail-fast NUMERIC_PARSE 3 11 "cannot parse numeric input as int8: value out of range" <<'QUI'
string text := "300"
print("before" + NL)
int8 n := int8(text)
QUI
details parse-details '{"code": "NUMERIC_PARSE", "details": {"type": "nat16", "reason": "INVALID_TEXT"}}' <<'QUI'
string text := "x"
nat16 n := nat16(text)
QUI

# --- File operations keep their cause: the operation's code, the path as
# the program passed it, and the reason.
mkdir -p "$TMP/files/existing-directory"
printf 'text' > "$TMP/files/plain.txt"
prints file-causes "$(printf '%s\n' \
    "cannot open \"$TMP/files/missing.txt\": file not found" \
    "cannot open \"$TMP/files/existing-directory\": path is a directory" \
    "cannot create directory \"$TMP/files/existing-directory\": path already exists" \
    "cannot open for appending \"$TMP/files/existing-directory\": path is a directory" \
    "cannot copy \"$TMP/files/missing.txt\" to \"$TMP/files/copy.txt\": file not found" \
    "cannot open \"\": file not found")" <<QUI

void show(void | error result)
    match result
        void
            print("ok" + NL)
        error problem
            print(problem)
            print(NL)
match file.read("$TMP/files/missing.txt")
    file.read stream
        print("opened")
    error problem
        print(problem)
        print(NL)
match file.read("$TMP/files/existing-directory")
    file.read stream
        print("opened")
    error problem
        print(problem)
        print(NL)
show(file.mkdir("$TMP/files/existing-directory"))
match file.append("$TMP/files/existing-directory")
    file.append writer
        print("opened" + NL)
    error problem
        print(problem)
        print(NL)
show(file.copy("$TMP/files/missing.txt", "$TMP/files/copy.txt"))
match file.read("")
    file.read stream
        print("opened")
    error problem
        print(problem)
        print(NL)
QUI
details file-details "{\"code\": \"FILE_OPEN\", \"details\": {\"path\": \"$TMP/files/missing.txt\", \"mode\": \"READ\", \"reason\": \"NOT_FOUND\"}}" <<QUI
file.read reader := file.read("$TMP/files/missing.txt")
QUI

# --- The parts of an error: code, message, cause and the details by name;
# a detail of another schema reads none.
prints members "$(printf 'CONFIG_PORT\ninvalid port\nNUMERIC_PARSE\ncannot parse numeric input as int\nint\nINVALID_TEXT\nno path\nno text\ninvalid port')" <<'QUI'
int | error read_port(string text)
    match int(text)
        int port
            return port
        error problem
            return error("invalid port", cause = problem, code = "CONFIG_PORT")
match read_port("x")
    int port
        print(port)
    error problem
        print(problem.code + NL)
        print(problem.message + NL)
        match problem.cause
            none
                print("no cause" + NL)
            error inner
                print(inner.code + NL)
                print(inner.message + NL)
                match inner.type
                    string spelled
                        print(spelled + NL)
                    none
                        print("no type" + NL)
                match inner.reason
                    string reason
                        print(reason + NL)
                    none
                        print("no reason" + NL)
                match inner.path
                    string path
                        print(path + NL)
                    none
                        print("no path" + NL)
                match inner.text
                    string text
                        print(text + NL)
                    none
                        print("no text" + NL)
        match problem.text
            string text
                print(text + NL)
            none
                print("no text" + NL)
QUI
prints error-code-default "$(printf 'ERROR\n5')" <<'QUI'
error plain := error("plain")
print(plain.code + NL)
match json.parse("[1, 2")
    json.Value value
        print("parsed" + NL)
    error problem
        match problem.offset
            nat offset
                print(offset)
            none
                print("no offset")
QUI

# --- Equality: the code, the details and the cause chain; never the origin.
prints equality "$(printf 'true\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\nfalse')" <<'QUI'
class Holder this(local error held_initial)
    error held

    this.held = held_initial
error a := error("same", code = "SAME_CODE")
error b := error("same", code = "SAME_CODE")
error c := error("same")
print("{a == b}{NL}")
print("{a == c}{NL}")
print("{a != c}{NL}")
error wrapped_a := error("outer", cause = a)
error wrapped_b := error("outer", cause = b)
error wrapped_c := error("outer", cause = c)
print("{wrapped_a == wrapped_b}{NL}")
print("{wrapped_a == wrapped_c}{NL}")
error core := error("x")
match int("x")
    int value
        print(value)
    error parsed
        core = parsed
error user := error("cannot parse numeric input as int")
print("{core == user}{NL}")
match int("y")
    int value
        print(value)
    error again
        print("{core == again}{NL}")
error[] first := [a, wrapped_a]
error[] second := [b, wrapped_b]
error[] third := [b, wrapped_c]
error[] shorter := [a]
print("{first == second}{NL}")
print("{first == third}{NL}")
print("{first == shorter}{NL}")
print("{Holder(wrapped_a) == Holder(wrapped_b)}{NL}")
print("{Holder(wrapped_a) == Holder(wrapped_c)}{NL}")
QUI

# The details of a Core error take part: two reads of different paths are
# unequal, two of the same path made at two places equal.
prints file-equality "$(printf 'true\nfalse')" <<QUI
error | none read_error(string path)
    match file.read(path)
        file.read stream
            return none
        error problem
            return problem
error | none one := read_error("$TMP/missing-a.txt")
error | none two := read_error("$TMP/missing-a.txt")
error | none other := read_error("$TMP/missing-b.txt")
match one
    none
        print("none")
    error first
        match two
            none
                print("none")
            error second
                print("{first == second}{NL}")
        match other
            none
                print("none")
            error third
                print("{first == third}{NL}")
QUI

# --- check rows: ERROR_CODE, UNKNOWN_MEMBER, assignment, typed details.
# rejects NAME CODE < PROGRAM: `quidra check` reports CODE.
rejects() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    if QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.check" 2>&1; then
        fail "$name: accepted"; return 0
    fi
    if ! grep -q "error\[$code\]" "$TMP/$name.check"; then
        fail "$name: expected $code"; cat "$TMP/$name.check"; return 0
    fi
    echo "ok   $name"
}
accepts() {
    local name=$1
    cat > "$TMP/$name.qui"
    if ! QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.check" 2>&1; then
        fail "$name: rejected"; cat "$TMP/$name.check"; return 0
    fi
    echo "ok   $name"
}
code_row() {
    local name=$1 spelled=$2
    rejects "code-$name" ERROR_CODE <<QUI
error e := error("x", code = $spelled)
QUI
}
code_row variable 'spelling'
code_row template '"A{1}"'
code_row lower '"lower"'
code_row digit '"9LIVES"'
code_row leading '"_LEADING"'
code_row trailing '"TRAILING_"'
code_row double '"DOUBLE__UNDERSCORE"'
code_row index-bounds '"INDEX_BOUNDS"'
code_row file-read '"FILE_READ"'
code_row error '"ERROR"'
accepts code-long <<QUI
error e := error("x", code = "$(printf 'A%.0s' $(seq 1 200))")
QUI
rejects unknown-member UNKNOWN_MEMBER <<'QUI'
error e := error("x")
string s := e.size
QUI
rejects unknown-detail UNKNOWN_MEMBER <<'QUI'
error e := error("x")
string | none s := e.detail
QUI
# A member of an error is not an assignable target (the ordinary rule for
# a member of a value that is not a class).
rejects assign-detail TYPE_MISMATCH <<'QUI'
error e := error("x")
e.path = "y"
QUI
rejects assign-message TYPE_MISMATCH <<'QUI'
error e := error("x")
e.message = "y"
QUI
rejects detail-needs-match TYPE_MISMATCH <<'QUI'
error e := error("x")
bool missing := e.reason == "NOT_FOUND"
QUI
rejects cause-not-error TYPE_MISMATCH <<'QUI'
error e := error("x", cause = "text")
QUI
rejects cause-optional TYPE_MISMATCH <<'QUI'
error | none maybe := none
error e := error("x", cause = maybe)
QUI
mkdir -p "$TMP/packages/code_pkg"
cat > "$TMP/packages/code_pkg/quidra.package" <<'MANIFEST'
name = code_pkg
version = 0.1.0
MANIFEST
cat > "$TMP/packages/code_pkg/main.qui" <<'QUI'
int | error checked(int value)
    if value < 0
        return error("negative", code = "CODE_PKG_NEGATIVE")
    return value
QUI
accepts package-prefix <<'QUI'
import pkg = code_pkg

int n := pkg.checked(1)
QUI
mkdir -p "$TMP/packages/bad_pkg"
cat > "$TMP/packages/bad_pkg/quidra.package" <<'MANIFEST'
name = bad_pkg
version = 0.1.0
MANIFEST
cat > "$TMP/packages/bad_pkg/main.qui" <<'QUI'
int | error checked(int value)
    if value < 0
        return error("negative", code = "NEGATIVE")
    return value
QUI
rejects package-no-prefix ERROR_CODE <<'QUI'
import pkg = bad_pkg

int n := pkg.checked(1)
QUI

mkdir -p "$TMP/packages/codeless_pkg"
cat > "$TMP/packages/codeless_pkg/quidra.package" <<'MANIFEST'
name = codeless_pkg
version = 0.1.0
MANIFEST
cat > "$TMP/packages/codeless_pkg/main.qui" <<'QUI'
error problem()
    return error("missing code")
QUI
rejects package-codeless ERROR_CODE <<'QUI'
import pkg = codeless_pkg
error saved := pkg.problem()
QUI
cat >> "$TMP/packages/code_pkg/main.qui" <<'QUI'

error forward(error problem)
    return problem
QUI
accepts package-passthrough-error <<'QUI'
import pkg = code_pkg
error original := error("user failure")
error returned := pkg.forward(original)
QUI

# Insert after accepts/rejects definitions and before final failure summary.
# Operators consume a fallible success in their operand domain; they do not
# narrow ordinary unions, and full same-union equality remains union equality.
prints operand-success-domains $'3\n3\n-3\ntrue\ntrue\ntrue\n9\n3\n5:6\ntrue\nhandled' <<'QUI'
bool | error flag()
    return true
int[] | error items()
    return [4, 5]
println(int("1") + int("2"))
println(int8("1") + 2)
println(-int("3"))
println(not flag() or flag())
println(int("1") < int("2"))
println(int("1") < int("2") < int("3"))
int[] values := [8, 9]
println(values[int("1")])
println(int64(int("3")))
int[] shifted := items() + 1
println("{shifted[0]}:{shifted[1]}")
int | error problem := error("kept")
println(problem == problem)
match problem
    int value
        println(value)
    error failure
        println("handled")
QUI
prints operand-short-circuit $'false\ntrue\nleft\nmiddle\nfalse' <<'QUI'
bool | error skipped()
    println("unexpected")
    return error("skipped")
int | error value(string tag, int number)
    println(tag)
    return number
println(false and skipped())
println(true or skipped())
println(value("left", 3) < value("middle", 2) < value("unexpected", 1))
QUI
stops operand-error-before-right ERROR 3 12 'left failed' <<'QUI'
int | error left()
    println("before")
    return error("left failed")
int right()
    println("unexpected")
    return 2
println(left() + right())
QUI
stops array-operand-error-before-right ERROR 3 12 'array failed' <<'QUI'
int[] | error left()
    println("before")
    return error("array failed")
int[] right()
    println("unexpected")
    return [2]
values := left() + right()
QUI
stops index-base-error-before-index ERROR 3 12 'base failed' <<'QUI'
int[] | error base()
    println("before")
    return error("base failed")
int index()
    println("unexpected")
    return 0
println(base()[index()])
QUI
stops cast-source-error ERROR 3 12 'source failed' <<'QUI'
int | error source()
    println("before")
    return error("source failed")
println(int64(source()))
QUI
rejects ordinary-union-operand TYPE_MISMATCH <<'QUI'
int | real64 value := 1
println(value + 1)
QUI
rejects optional-fallible-operand TYPE_MISMATCH <<'QUI'
int | none | error value := none
println(value + 1)
QUI
rejects multi-success-fallible-operand TYPE_MISMATCH <<'QUI'
int | real64 | error value := 1
println(value + 1)
QUI
rejects optional-cast-source NUMERIC_CAST <<'QUI'
int | none | error value := none
println(int64(value))
QUI
prints fallible-value-iteration-and-bin $'4\n5\n5:6\n8\n10\n8\n16' <<'QUI'
int[] | error items()
    return [4, 5]
nat8[] | error bytes()
    return [1, 2]
for value := items()[]
    println(value)
result := [for value := items()[] then value + 1]
println("{result[0]}:{result[1]}")
for left := items()[], right := items()[]
    println(left + right)
nat number := 3
println(bin(nat8(number)).len())
println(bin(bytes()).len())
QUI
stops foreach-source-error ERROR 3 12 'source failed' <<'QUI'
int[] | error values()
    println("before")
    return error("source failed")
for value := values()[]
    println("unexpected")
QUI
stops comprehension-source-error ERROR 3 12 'source failed' <<'QUI'
int[] | error values()
    println("before")
    return error("source failed")
result := [for value := values()[] then value + 1]
println("unexpected")
QUI
stops multi-foreach-source-error-before-next ERROR 3 12 'source failed' <<'QUI'
int[] | error first()
    println("before")
    return error("source failed")
int[] second()
    println("unexpected")
    return [2]
for left := first()[], right := second()[]
    println("unexpected")
QUI
stops bin-source-error ERROR 3 12 'source failed' <<'QUI'
nat8 | error source()
    println("before")
    return error("source failed")
bits := bin(source())
println("unexpected")
QUI
rejects optional-foreach-source TYPE_MISMATCH <<'QUI'
int[] | none | error values()
    return none
for value := values()[]
    println(value)
QUI
rejects optional-comprehension-source ARRAY_COMPREHENSION <<'QUI'
int[] | none | error values()
    return none
result := [for value := values()[] then value]
QUI
rejects ordinary-union-foreach-source TYPE_MISMATCH <<'QUI'
int[] | nat[] values()
    return [1]
for value := values()[]
    println(value)
QUI
rejects ordinary-union-bin-source TYPE_MISMATCH <<'QUI'
nat8 | bool value := true
bits := bin(value)
QUI
rejects optional-bin-source TYPE_MISMATCH <<'QUI'
nat8 | none | error value := none
bits := bin(value)
QUI
rejects fallible-reference-iteration TYPE_MISMATCH <<'QUI'
int[] | error values()
    return [1, 2]
for int &value := &values()[]
    value = 4
QUI
# Uncontextualized nonliteral if branches still require identical types.
rejects inferred-if-success-fallible-branches TYPE_MISMATCH <<'QUI'
int value := 1
result := if true then value else int("2")
QUI
prints contextual-if-fallible-branch $'1\nhandled' <<'QUI'
int value := 1
int selected := if true then value else int("bad")
println(selected)
int | error result := if false then value else int("bad")
match result
    int number
        println(number)
    error failure
        println("handled")
QUI

# Exact runtime owner counts catch a retained payload even if a small allocator
# pool makes RSS insensitive. This weak observation never retains/releases it.
cat > "$TMP/owner-probe.cpp" <<'CPP'
#include <cstddef>
#include <cstdint>
namespace quidra::runtime { std::size_t managed_owners(const void*); }
static const char* remembered;
extern "C" std::uint64_t qtest_string_owners(const char* text, std::uint64_t) {
    return quidra::runtime::managed_owners(text);
}
extern "C" void qtest_string_remember(const char* text, std::uint64_t) {
    remembered = text;
}
extern "C" std::uint64_t qtest_string_remaining() {
    return quidra::runtime::managed_owners(remembered);
}
CPP
if ! "${CXX:-c++}" -std=c++20 -c "$TMP/owner-probe.cpp" -o "$TMP/owner-probe.o"; then
    fail "owner probe: C++ compilation failed"
else
    owner_prints() {
        build_extra_arguments=(--link "$TMP/owner-probe.o")
        prints "$@"
        build_extra_arguments=()
    }
    owner_prints fail-fast-owned-success-borrowed-root $'aaaaaaaa\n0\n8\n0\naaaaaaaa\n0\n8\n0\naaaaaaaa\n0' <<'QUI'
extern nat64 owners(const string text) = "qtest_string_owners"
extern void remember(const string text) = "qtest_string_remember"
extern nat64 remaining() = "qtest_string_remaining"
class Holder()
    string | error text := string.repeat("a", 8)
void inspect_name()
    string | error text := string.repeat("a", 8)
    remember(text)
    nat64 before := owners(text)
    println(text)
    println(owners(text) - before)
    match text
        string original
            println(original.len())
        error problem
            println("unexpected")
inspect_name()
println(remaining())
void inspect_member()
    holder := Holder()
    remember(holder.text)
    nat64 before := owners(holder.text)
    println(holder.text)
    println(owners(holder.text) - before)
    match holder.text
        string original
            println(original.len())
        error problem
            println("unexpected")
inspect_member()
println(remaining())
string | error owned()
    string text := string.repeat("a", 8)
    remember(text)
    return text
println(owned())
println(remaining())
QUI
    owner_prints string-prefix-try-cleanup 0 <<'QUI'
extern void remember(const string text) = "qtest_string_remember"
extern nat64 remaining() = "qtest_string_remaining"
string left()
    string text := string.repeat("a", 1024)
    remember(text)
    return text
string | error right()
    return error("stop")
string | error compute()
    return left() + try right()
match compute()
    string result
        println("unexpected")
    error problem
        println(remaining())
QUI
    owner_prints eager-operand-try-cleanup 0 <<'QUI'
extern void remember(const string text) = "qtest_string_remember"
extern nat64 remaining() = "qtest_string_remaining"
class Payload()
    string text := string.repeat("a", 1024)
Payload make()
    value := Payload()
    remember(value.text)
    return value
Payload | error stop()
    return error("stop")
bool | error compute()
    return make() == try stop()
match compute()
    bool result
        println("unexpected")
    error problem
        println(remaining())
QUI
    owner_prints comparison-first-prefix-try-cleanup 0 <<'QUI'
extern void remember(const string text) = "qtest_string_remember"
extern nat64 remaining() = "qtest_string_remaining"
class Payload()
    string text := string.repeat("a", 1024)
Payload make()
    value := Payload()
    remember(value.text)
    return value
Payload | error stop()
    return error("stop")
bool | error compute()
    return make() == try stop() == make()
match compute()
    bool result
        println("unexpected")
    error problem
        println(remaining())
QUI
    owner_prints comparison-middle-prefix-try-cleanup 0 <<'QUI'
extern void remember(const string text) = "qtest_string_remember"
extern nat64 remaining() = "qtest_string_remaining"
class Payload()
    string text := string.repeat("a", 1024)
Payload make()
    value := Payload()
    remember(value.text)
    return value
Payload | error stop()
    return error("stop")
bool | error compute()
    return make() == make() == try stop()
match compute()
    bool result
        println("unexpected")
    error problem
        println(remaining())
QUI
    owner_prints consumed-managed-operand-owner-balance $'9\n0\n8\n0\n8\n0\n8\n0\n8\n8\n0' <<'QUI'
extern nat64 owners(const string text) = "qtest_string_owners"
extern void remember(const string text) = "qtest_string_remember"
extern nat64 remaining() = "qtest_string_remaining"
void inspect()
    string original := string.repeat("a", 8)
    string | error text := original
    string[] | error values := [original]
    remember(original)
    nat64 before := owners(original)
    string joined := text + "!"
    println(joined.len())
    println(owners(original) - before)
    println(values[0].len())
    println(owners(original) - before)
    for value := values[]
        println(value.len())
    println(owners(original) - before)
    counts := [for value := values[] then value.len()]
    println(counts[0])
    println(owners(original) - before)
    match text
        string kept
            println(kept.len())
        error problem
            println("unexpected")
    match values
        string[] kept
            println(kept[0].len())
        error problem
            println("unexpected")
inspect()
println(remaining())
QUI
fi
prints string-prefix-survives-root-replacement $'1025\na\nb' <<'QUI'
string replace(string &text)
    text = string.repeat("b", 1024)
    return "!"
string text := string.repeat("a", 1024)
string joined := text + replace(&text)
println(joined.len())
println(joined[0])
println(text[0])
QUI
prints ascii-index-base-survives-index-replacement $'true\nb' <<'QUI'
int choose(string &text)
    text = string.repeat("b", 1024)
    return 0
string text := string.repeat("a", 1024)
println(text[choose(&text)] == "a")
println(text[0])
QUI

if [[ "$failures" -ne 0 ]]; then
    echo "$failures error value row(s) failed"
    exit 1
fi
echo "error value tests passed"
