#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/quidra-stream-cache"

cat > "$TMP/stream-v2.qui" <<QUI
void write_sample(string path)
    file.write output(path)
    .output("alpha{NL}beta")
void append_sample(string path)
    file.append appended(path)
    .appended("{NL}gamma")
write_sample("$TMP/stream-v2.txt")
append_sample("$TMP/stream-v2.txt")
file.read source("$TMP/stream-v2.txt")
while .source(string &line)
    print("{line}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-v2.qui")" == "$(printf 'alpha\nbeta\ngamma')" ]]

cat > "$TMP/callable-v2.qui" <<'QUI'
class Counter(int value)
    int .operator()
        value += 1
        return value

Counter counter = Counter(0)
print(.counter())
print(",")
print(.counter())
QUI
[[ "$("$QUIDRA" "$TMP/callable-v2.qui")" == "1,2" ]]

cat > "$TMP/condition-v2.qui" <<'QUI'
bool parse(string source, int &output)
    if source == "valid"
        output = 42
        return true
    return false

if parse("valid", int &number)
    print(number)
QUI
[[ "$("$QUIDRA" "$TMP/condition-v2.qui")" == "42" ]]

cat > "$TMP/invalid-condition-v2.qui" <<'QUI'
bool invalid(int &output)
    return true

if invalid(int &number)
    print(number)
QUI
if "$QUIDRA" check "$TMP/invalid-condition-v2.qui" >"$TMP/invalid-condition-v2.out" 2>"$TMP/invalid-condition-v2.err"; then
    echo "Conditional uninitialized reference was incorrectly accepted" >&2
    exit 1
fi

cat > "$TMP/deprecated-stream-surface.qui" <<'QUI'
auto handle = file.open("legacy.txt")
QUI
if "$QUIDRA" check "$TMP/deprecated-stream-surface.qui" >"$TMP/deprecated-stream.out" 2>"$TMP/deprecated-stream.err"; then
    echo "Obsolete file.open() remains publicly accessible" >&2
    exit 1
fi

cat > "$TMP/deprecated-handle-type.qui" <<'QUI'
file.Handle handle
QUI
if "$QUIDRA" check "$TMP/deprecated-handle-type.qui" >"$TMP/deprecated-handle.out" 2>"$TMP/deprecated-handle.err"; then
    echo "Obsolete file.Handle type remains publicly accessible" >&2
    exit 1
fi

cat > "$TMP/deprecated-whole-text.qui" <<'QUI'
string content = file.read("legacy.txt")
QUI
if "$QUIDRA" check "$TMP/deprecated-whole-text.qui" >"$TMP/deprecated-whole.out" 2>"$TMP/deprecated-whole.err"; then
    echo "Obsolete whole-text file.read() type remains accepted" >&2
    exit 1
fi

cat > "$TMP/deprecated-write-arity.qui" <<'QUI'
file.write("legacy.txt", "hello")
QUI
if "$QUIDRA" check "$TMP/deprecated-write-arity.qui" >"$TMP/deprecated-write.out" 2>"$TMP/deprecated-write.err"; then
    echo "Obsolete file.write(path,text) overload remains accepted" >&2
    exit 1
fi

cat > "$TMP/read-only-operator.qui" <<'QUI'
class Addition(int offset)
    int operator(int value)
        return offset + value

Addition add = Addition(7)
print(add(5))
QUI
[[ "$("$QUIDRA" "$TMP/read-only-operator.qui")" == "12" ]]

cat > "$TMP/missing-mutation-marker.qui" <<'QUI'
class Counter(int value)
    int .operator()
        value += 1
        return value

Counter c = Counter(0)
print(c())
QUI
if "$QUIDRA" check "$TMP/missing-mutation-marker.qui" >"$TMP/missing-mutation-marker.out" 2>"$TMP/missing-mutation-marker.err"; then
    echo "Mutating operator was accepted without the required dot marker" >&2
    exit 1
fi

cat > "$TMP/conditional-reference-scope.qui" <<'QUI'
bool parse(int &output)
    output = 17
    return true

if parse(int &value)
    print(value)
print(value)
QUI
if "$QUIDRA" check "$TMP/conditional-reference-scope.qui" >"$TMP/conditional-reference-scope.out" 2>"$TMP/conditional-reference-scope.err"; then
    echo "Inline output variable escaped its conditional scope" >&2
    exit 1
fi

cat > "$TMP/conditional-success-only.qui" <<'QUI'
bool parse(bool valid, int &output)
    if valid
        output = 17
        return true
    return false

if parse(true, int &value)
    print(value)
QUI
[[ "$("$QUIDRA" "$TMP/conditional-success-only.qui")" == "17" ]]

printf '\377\n' > "$TMP/invalid-utf8.txt"
cat > "$TMP/stream-invalid-utf8.qui" <<QUI
file.read source = file.read("$TMP/invalid-utf8.txt")
while .source(string &line)
    print(line)
QUI
if "$QUIDRA" "$TMP/stream-invalid-utf8.qui" >"$TMP/stream-invalid-utf8.out" 2>"$TMP/stream-invalid-utf8.err"; then
    echo "Invalid UTF-8 in the stream was treated as EOF" >&2
    exit 1
fi
# Exact public shorthand and boundary cases: empty lines, final unterminated
# lines, and EOF must be distinguishable from I/O failure.
cat > "$TMP/stream-short-form.qui" <<QUI
void write_lines(string path)
    file.write f = file.write(path)
    .f("first{NL}{NL}third")
write_lines("$TMP/stream-short-form.txt")
file.read f = file.read("$TMP/stream-short-form.txt")
int number = 0
while .f(string &line)
    number += 1
    print("{number}:{line}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-short-form.qui")" == "$(printf '1:first\n2:\n3:third')" ]]

cat > "$TMP/stream-empty.qui" <<QUI
file.write writer = file.write("$TMP/stream-empty.txt")
file.read reader = file.read("$TMP/stream-empty.txt")
int lines = 0
while .reader(string &line)
    lines += 1
print(lines)
QUI
[[ "$("$QUIDRA" "$TMP/stream-empty.qui")" == "0" ]]

# Typed scalar reads consume one complete line per call.
printf '42\n-7\n2.5\ntrue\nfalse' > "$TMP/stream-typed.txt"
cat > "$TMP/stream-typed.qui" <<QUI
file.read source("$TMP/stream-typed.txt")
if .source(int &exact)
    print("i:{exact}|")
if .source(int64 &fixed)
    print("n:{fixed}|")
if .source(real64 &decimal)
    print("r:{decimal}|")
if .source(bool &yes)
    print("b:{yes}|")
if .source(bool &no)
    print("c:{no}|")
int preserved = 10
bool eof = .source(&preserved)
print("eof:{eof}|preserved:{preserved}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-typed.qui")" == "i:42|n:-7|r:2.5|b:true|c:false|eof:false|preserved:10" ]]

printf '1\n2\n3' > "$TMP/stream-ints.txt"
cat > "$TMP/stream-ints.qui" <<QUI
file.read source("$TMP/stream-ints.txt")
while .source(int &value)
    print("{value},")
QUI
[[ "$("$QUIDRA" "$TMP/stream-ints.qui")" == "1,2,3," ]]

# Conversion errors are not EOF and must not overwrite the output.
printf '10 20' > "$TMP/stream-invalid-number.txt"
cat > "$TMP/stream-invalid-number.qui" <<QUI
file.read source("$TMP/stream-invalid-number.txt")
int preserved = 99
match .source(&preserved)
    bool success
        print("unexpected:{success}")
    error problem
        print("{problem.code}:{preserved}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-invalid-number.qui")" == "NUMERIC_PARSE:99" ]]

printf '128' > "$TMP/stream-overflow.txt"
cat > "$TMP/stream-overflow.qui" <<QUI
file.read source("$TMP/stream-overflow.txt")
int8 preserved = 7
match .source(&preserved)
    bool success
        print("unexpected:{success}")
    error problem
        print("{problem.code}:{preserved}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-overflow.qui")" == "NUMERIC_PARSE:7" ]]

printf 'TRUE' > "$TMP/stream-invalid-bool.txt"
cat > "$TMP/stream-invalid-bool.qui" <<QUI
file.read source("$TMP/stream-invalid-bool.txt")
bool preserved = true
match .source(&preserved)
    bool success
        print("unexpected:{success}")
    error problem
        print("{problem.code}:{preserved}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-invalid-bool.qui")" == "NUMERIC_PARSE:true" ]]

# Numeric literals are accepted directly by writer and appender.
cat > "$TMP/stream-write-literals.qui" <<QUI
void write_literals(string path)
    file.write output(path)
    .output(42)
    .output("|")
    .output(-7)
    .output("|")
    .output(1.25)
    .output("|")
    .output(true)
write_literals("$TMP/stream-write-literals.txt")
file.read source("$TMP/stream-write-literals.txt")
if .source(string &line)
    print(line)
QUI
[[ "$("$QUIDRA" "$TMP/stream-write-literals.qui")" == "42|-7|1.25|true" ]]
printf '42|-7|1.25|true' > "$TMP/stream-write-literals.expected"
cmp -s "$TMP/stream-write-literals.expected" "$TMP/stream-write-literals.txt"

# Writers and appenders accept numeric/bool scalar values without adding line breaks.
cat > "$TMP/stream-write-typed.qui" <<QUI
void write_typed(string path)
    file.write output(path)
    int big = 9007199254740993
    nat8 byte = 255
    real64 decimal = 2.5
    bool enabled = true
    .output(big)
    .output("{NL}")
    .output(byte)
    .output("{NL}")
    .output(decimal)
    .output("{NL}")
    .output(enabled)
    .output("{NL}")
    .output(false)
    .output("{NL}")
void append_typed(string path)
    file.append output(path)
    .output(-7)
    .output("{NL}")
write_typed("$TMP/stream-write-typed.txt")
append_typed("$TMP/stream-write-typed.txt")
file.read source("$TMP/stream-write-typed.txt")
if .source(int &big)
    print("{big}|")
if .source(nat8 &byte)
    print("{byte}|")
if .source(real64 &decimal)
    print("{decimal}|")
if .source(bool &enabled)
    print("{enabled}|")
if .source(bool &disabled)
    print("{disabled}|")
if .source(int &negative)
    print("{negative}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-write-typed.qui")" == "9007199254740993|255|2.5|true|false|-7" ]]
printf '9007199254740993\n255\n2.5\ntrue\nfalse\n-7\n' > "$TMP/stream-write-typed.expected"
cmp -s "$TMP/stream-write-typed.expected" "$TMP/stream-write-typed.txt"

cat > "$TMP/stream-write-invalid-target.qui" <<'QUI'
file.write output("unused.txt")
.output([1, 2])
QUI
if "$QUIDRA" check "$TMP/stream-write-invalid-target.qui" >"$TMP/stream-write-invalid-target.out" 2>"$TMP/stream-write-invalid-target.err"; then
    echo "File write accepted an unsupported array value" >&2
    exit 1
fi

cat > "$TMP/stream-invalid-target.qui" <<'QUI'
file.read source("unused.txt")
if .source(string[] &values)
    print(len(values))
QUI
if "$QUIDRA" check "$TMP/stream-invalid-target.qui" >"$TMP/stream-invalid-target.out" 2>"$TMP/stream-invalid-target.err"; then
    echo "File read accepted an unsupported non-scalar output type" >&2
    exit 1
fi

# Formatted file reads share scan's literal delimiters, typed output variables
# and exact one-line boundary with normal false-at-EOF iteration.
printf '10 20\n30 40' > "$TMP/stream-format-lines.txt"
cat > "$TMP/stream-format-lines.qui" <<QUI
file.read input("$TMP/stream-format-lines.txt")
while .input("{int &left} {int &right}")
    print("{left}+{right}|")
QUI
[[ "$("$QUIDRA" "$TMP/stream-format-lines.qui")" == "10+20|30+40|" ]]

cat > "$TMP/stream-format-mixed.qui" <<QUI
file.read input("$TMP/stream-format-lines.txt")
int first = -1
if .input("{&first} {int &second}")
    print("{first}:{second}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-format-mixed.qui")" == "10:20" ]]

# Formatting a line containing invalid data does not commit any target.
printf '10 invalid\n' > "$TMP/stream-format-bad.txt"
cat > "$TMP/stream-format-bad.qui" <<QUI
file.read input("$TMP/stream-format-bad.txt")
int first = 90
int second = 80
match .input("{&first} {&second}")
    bool ok
        print("unexpected:{ok}")
    error problem
        print("{problem.code}:{first}:{second}")
QUI
[[ "$("$QUIDRA" "$TMP/stream-format-bad.qui")" == "INPUT_FORMAT:90:80" ]]

printf 'true,5\nfalse,6' > "$TMP/stream-format-bools.txt"
cat > "$TMP/stream-format-bools.qui" <<QUI
file.read input("$TMP/stream-format-bools.txt")
while .input("{bool &flag},{int &count}")
    print("{flag}:{count}|")
QUI
[[ "$("$QUIDRA" "$TMP/stream-format-bools.qui")" == "true:5|false:6|" ]]

# A standalone scan statement may declare an output binding after fail-fast,
# but the declaration of a file-reader condition is scoped to its branch.
cat > "$TMP/scan-inline-input.qui" <<'QUI'
int first = 0
scan("{&first} {int &second}")
print("{first}:{second}")
QUI
[[ "$(printf '11 12\n' | "$QUIDRA" "$TMP/scan-inline-input.qui")" == "11:12" ]]

cat > "$TMP/scan-boolean-input.qui" <<'QUI'
scan("{bool &enabled} {int &value}")
print("{enabled}:{value}")
QUI
[[ "$(printf 'true 13\n' | "$QUIDRA" "$TMP/scan-boolean-input.qui")" == "true:13" ]]

cat > "$TMP/scan-atomic-failure.qui" <<'QUI'
int first = 90
int second = 80
void | error result = scan("{&first} {&second}")
match result
    void
        print("unexpected")
    error problem
        print("{problem.code}:{first}:{second}")
QUI
[[ "$(printf '10 bad\n' | "$QUIDRA" "$TMP/scan-atomic-failure.qui")" == "INPUT_FORMAT:90:80" ]]

cat > "$TMP/stream-format-adjacent.qui" <<'QUI'
file.read input("unused.txt")
while .input("{int &first}{int &second}")
    print(first)
QUI
if "$QUIDRA" check "$TMP/stream-format-adjacent.qui" >"$TMP/stream-format-adjacent.out" 2>"$TMP/stream-format-adjacent.err"; then
    echo "Adjacent undelimited input targets were accepted" >&2
    exit 1
fi

cat > "$TMP/stream-format-scope.qui" <<'QUI'
file.read input("unused.txt")
while .input("{int &first} {int &second}")
    print(first)
print(second)
QUI
if "$QUIDRA" check "$TMP/stream-format-scope.qui" >"$TMP/stream-format-scope.out" 2>"$TMP/stream-format-scope.err"; then
    echo "Formatted while input target escaped its branch" >&2
    exit 1
fi

# The sole user-defined call operator belongs to the normal method-name
# namespace: mutable/immutable variants cannot be overloaded.
cat > "$TMP/duplicate-call-operator.qui" <<'QUI'
class Counter(int value)
    int operator()
        return value
    int .operator()
        value += 1
        return value
QUI
if "$QUIDRA" check "$TMP/duplicate-call-operator.qui" >"$TMP/duplicate-call-operator.out" 2>"$TMP/duplicate-call-operator.err"; then
    echo "Read-only/mutating call operators were incorrectly overloaded" >&2
    exit 1
fi

# Inline output declarations exist only on the successful condition edge.
cat > "$TMP/conditional-else-scope.qui" <<'QUI'
bool parse(int &output)
    output = 42
    return true
if parse(int &value)
    print(value)
else
    print(value)
QUI
if "$QUIDRA" check "$TMP/conditional-else-scope.qui" >"$TMP/conditional-else-scope.out" 2>"$TMP/conditional-else-scope.err"; then
    echo "Condition output variable escaped into else branch" >&2
    exit 1
fi

cat > "$TMP/while-output-scope.qui" <<QUI
file.read f = file.read("$TMP/stream-short-form.txt")
while .f(string &line)
    print(line)
print(line)
QUI
if "$QUIDRA" check "$TMP/while-output-scope.qui" >"$TMP/while-output-scope.out" 2>"$TMP/while-output-scope.err"; then
    echo "Condition output variable escaped from while body" >&2
    exit 1
fi

# The reader and writer have distinct static call contracts.
cat > "$TMP/writer-as-reader.qui" <<'QUI'
file.write output = file.write("somewhere.txt")
while .output(string &line)
    print(line)
QUI
if "$QUIDRA" check "$TMP/writer-as-reader.qui" >"$TMP/writer-as-reader.out" 2>"$TMP/writer-as-reader.err"; then
    echo "Writer incorrectly accepted a read call" >&2
    exit 1
fi

cat > "$TMP/reader-as-writer.qui" <<'QUI'
file.read input = file.read("somewhere.txt")
.input("data")
QUI
if "$QUIDRA" check "$TMP/reader-as-writer.qui" >"$TMP/reader-as-writer.out" 2>"$TMP/reader-as-writer.err"; then
    echo "Reader incorrectly accepted a write call" >&2
    exit 1
fi


# File-mode names are nominal types; both direct and deferred initialization
# use the corresponding file namespace constructor.
cat > "$TMP/mode-typed-direct.qui" <<QUI
void write_direct(string path)
    file.write output(path)
    .output("old")
void append_direct(string path)
    file.append extra(path)
    .extra("{NL}new")
write_direct("$TMP/mode-typed-direct.txt")
append_direct("$TMP/mode-typed-direct.txt")
file.read input("$TMP/mode-typed-direct.txt")
while .input(string &line)
    print("{line}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/mode-typed-direct.qui")" == "$(printf 'old\nnew')" ]]

cat > "$TMP/mode-typed-deferred.qui" <<QUI
void write_deferred(string path)
    file.write writer
    writer = file.write(path)
    .writer("first")
void append_deferred(string path)
    file.append extra
    extra = file.append(path)
    .extra("second")
write_deferred("$TMP/mode-typed-deferred.txt")
append_deferred("$TMP/mode-typed-deferred.txt")
file.read source
source = file.read("$TMP/mode-typed-deferred.txt")
while .source(string &line)
    print(line)
QUI
[[ "$("$QUIDRA" "$TMP/mode-typed-deferred.qui")" == "firstsecond" ]]

cat > "$TMP/mode-cross-assign.qui" <<'QUI'
file.write destination
destination = file.append("log.txt")
QUI
if "$QUIDRA" check "$TMP/mode-cross-assign.qui" >"$TMP/mode-cross-assign.out" 2>"$TMP/mode-cross-assign.err"; then
    echo "An append stream was incorrectly assignable to a write stream" >&2
    exit 1
fi

cat > "$TMP/deprecated-file-shorthand.qui" <<'QUI'
file stream = file.read("input.txt")
QUI
if "$QUIDRA" check "$TMP/deprecated-file-shorthand.qui" >"$TMP/deprecated-file-shorthand.out" 2>"$TMP/deprecated-file-shorthand.err"; then
    echo "The deprecated unqualified file type was accepted" >&2
    exit 1
fi

cat > "$TMP/deprecated-reader-type.qui" <<'QUI'
file.Reader stream
QUI
if "$QUIDRA" check "$TMP/deprecated-reader-type.qui" >"$TMP/deprecated-reader-type.out" 2>"$TMP/deprecated-reader-type.err"; then
    echo "The removed file.Reader type was accepted" >&2
    exit 1
fi

cat > "$TMP/deprecated-writer-type.qui" <<'QUI'
file.Writer stream
QUI
if "$QUIDRA" check "$TMP/deprecated-writer-type.qui" >"$TMP/deprecated-writer-type.err" 2>&1; then
    echo "The removed file.Writer type was accepted" >&2
    exit 1
fi
