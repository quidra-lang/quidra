#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/quidra-line-io-cache"

# Exact stdout bytes, not command substitution (which strips trailing newlines).
cat > "$TMP/print-lines.qui" <<'QUI'
int value := 42
print("head:")
print.line("Hello")
print.line("answer={value}")
print.line(value)
print.line()
print("tail")
QUI
"$QUIDRA" "$TMP/print-lines.qui" > "$TMP/print-lines.out"
printf 'head:Hello\nanswer=42\n42\n\ntail' > "$TMP/print-lines.expected"
cmp "$TMP/print-lines.expected" "$TMP/print-lines.out"

cat > "$TMP/file-lines.qui" <<QUI
void create_lines(string path)
    file.write w := file.write(path)
    .w.line("First")
    .w("Second")
    .w.line()
    .w.line(42)
    .w.line(true)

void append_lines(string path)
    file.append a := file.append(path)
    .a.line("last")
    .a.line()

create_lines("$TMP/lines.txt")
append_lines("$TMP/lines.txt")
QUI
"$QUIDRA" "$TMP/file-lines.qui" > "$TMP/file-lines.out"
printf 'First\nSecond\n42\ntrue\nlast\n\n' > "$TMP/file-lines.expected"
cmp "$TMP/file-lines.expected" "$TMP/lines.txt"
[ ! -s "$TMP/file-lines.out" ]

# Existing unadorned output remains exactly unchanged.
cat > "$TMP/legacy.qui" <<QUI
print("a")
print("b")
file.write w := file.write("$TMP/legacy.txt")
.w("x")
.w("y")
QUI
"$QUIDRA" "$TMP/legacy.qui" > "$TMP/legacy.out"
printf 'ab' > "$TMP/legacy.expected.stdout"
printf 'xy' > "$TMP/legacy.expected.file"
cmp "$TMP/legacy.expected.stdout" "$TMP/legacy.out"
cmp "$TMP/legacy.expected.file" "$TMP/legacy.txt"

# Invalid input variants must be rejected at check time, not crash.
reject() {
    local id="$1" source="$2"
    printf '%s\n' "$source" > "$TMP/$id.qui"
    if "$QUIDRA" check "$TMP/$id.qui" > "$TMP/$id.out" 2> "$TMP/$id.err"; then
        echo "Unexpected acceptance of $id" >&2
        exit 1
    fi
}
reject print_wrong_arity 'print.line("x", "y")'
reject print_bad_type 'print.line([1, 2])'
reject print_bad_name 'print.line(unknown = "x")'
reject file_wrong_arity 'file.write w := file.write("missing.txt")
.w.line("x", "y")'
reject file_wrong_type 'file.write w := file.write("missing.txt")
.w.line([1, 2])'
reject read_no_line 'file.read r := file.read("missing.txt")
.r.line()'
reject missing_mutation_dot 'file.write w := file.write("missing.txt")
w.line("x")'
