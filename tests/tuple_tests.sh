#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/values.qui" <<'QUI'
(int, string) make()
    return (10, "a, b")
(int n, string text) = make()
print("{n} {text}{NL}")
int a = 1
int b = 2
(a, b) = (b, a)
print("{a} {b}{NL}")
((int a0, int b0), string c0) = ((a, b), text)
print("{a0} {b0} {c0}{NL}")
(int, string) pair = (a, text)
(int, string) copy = pair
(int &left, string &right) = &pair
left = 9
right = "changed"
print("{pair}{NL}{copy}{NL}")
print(pair == copy)
print(NL)
(bool, com32, nat8) aligned = (true, com32(1.0 + 2.0i), 3)
(bool ok, com32 z, nat8 count) = aligned
print(ok and z == com32(1.0 + 2.0i) and count == 3)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/values.qui")" == $'10 a, b\n2 1\n2 1 a, b\n(9, "changed")\n(2, "a, b")\nfalse\ntrue' ]]
cat > "$TMP/loops.qui" <<'QUI'
(int, string)[] pairs = [(1, "one"), (2, "two")]
for (int n, string s) = pairs[nat i]
    print("{i} {n} {s}{NL}")
for (int &n, string &s) = &pairs[]
    n += 10
    s = "x"
int existing = 0
string word = ""
for (existing, word) = pairs[]
    print("{existing} {word}{NL}")
for (_, _) = pairs[]
    print("discard{NL}")
pairs = pairs.append((13, "three"))
(int n, string s) = pairs[2]
print("{n} {s}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/loops.qui")" == $'0 1 one\n1 2 two\n11 x\n12 x\ndiscard\ndiscard\n13 three' ]]
cat > "$TMP/match.qui" <<'QUI'
(int, string) first_value = (1, "first")
(int, string) | (string, nat) | none value = first_value
match value
    (int n, auto text)
        print("{n} {text}{NL}")
    (string text, _)
        print(text)
    none
        print("none")
(int, real64)[2] | none array_value = [(1, 0.5), (2, 1.0)]
match array_value
    (int, real64)[2] values
        for (int n, real64 x) = values[]
            print("{n} {x}{NL}")
    none
        print("none")
((int, int), string) | none nested = ((1, 2), "nested")
match nested
    ((int a, int b), const string text)
        print("{a} {b} {text}{NL}")
    none
        print("none")
QUI
[[ "$("$QUIDRA" "$TMP/match.qui")" == $'1 first\n1 0.5\n2 1.0\n1 2 nested' ]]
cat > "$TMP/keys.qui" <<'QUI'
map.Map<(string, int), int> values
.values.set(("key", 1), 11)
.values.set(("key", 1), 12)
print(values.has(("key", 1)))
print(NL)
set.Set<(int, (string, bool))> keys
.keys.add((1, ("a", true)))
.keys.add((1, ("a", true)))
print(keys.has((1, ("a", true))))
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/keys.qui")" == $'true\ntrue' ]]
# Union stringification uses the same per-alternative rendering as
# interpolation, rather than emitting an undefined LLVM ToString result.
cat > "$TMP/union-string.qui" <<'QUI'
int | none present = 7
int | none missing = none
print(present.string())
print(NL)
print(missing.string())
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/union-string.qui")" == "$1" source="$2"
    printf '%s\n' "$source" > "$TMP/reject.qui"
    if "$QUIDRA" check "$TMP/reject.qui" --json > "$TMP/reject.json"; then exit 1; fi
    python3 - "$TMP/reject.json" "$code" <<'PYJSON'
import json,sys
assert any(d['code']==sys.argv[2] for d in json.load(open(sys.argv[1]))['diagnostics'])
PYJSON
}
check_rejected TUPLE $'(int, int) p = (1, 2)\nprint(p[0])'
check_rejected TUPLE $'(int, int) p = (1, 2)\nprint(p.string())'
check_rejected TUPLE $'int a = 1\n(a, a) = (1, 2)'
check_rejected TUPLE $'int a = 1\nint b = 2\n(a, b) += (1, 2)'
check_rejected UNINITIALIZED $'(string, int) p\n(string &s, int &n) = &p'
check_rejected MATCH_CASE $'(int, int) initial = (1, 2)\n(int, int) | (int, string) v = initial\nmatch v\n    (int a, _)\n        print(a)'
check_rejected MATCH_CASE $'(int, string) | none v = (1, "x")\nmatch v\n    (int &a, string &b)\n        print(a)\n    none\n        print("none")'
check_rejected DISCARD $'(int a, _) = (1, 2)'
for name in values loops match keys; do
    cp "$TMP/$name.qui" "$TMP/$name.formatted.qui"
    "$QUIDRA" fmt "$TMP/$name.formatted.qui"
    [[ "$("$QUIDRA" "$TMP/$name.qui")" == "$("$QUIDRA" "$TMP/$name.formatted.qui")" ]]
done
7\nnone' ]]

check_rejected() {
    local code="$1" source="$2"
    printf '%s\n' "$source" > "$TMP/reject.qui"
    if "$QUIDRA" check "$TMP/reject.qui" --json > "$TMP/reject.json"; then exit 1; fi
    python3 - "$TMP/reject.json" "$code" <<'PYJSON'
import json,sys
assert any(d['code']==sys.argv[2] for d in json.load(open(sys.argv[1]))['diagnostics'])
PYJSON
}
check_rejected TUPLE $'(int, int) p = (1, 2)\nprint(p[0])'
check_rejected TUPLE $'(int, int) p = (1, 2)\nprint(p.string())'
check_rejected TUPLE $'int a = 1\n(a, a) = (1, 2)'
check_rejected TUPLE $'int a = 1\nint b = 2\n(a, b) += (1, 2)'
check_rejected UNINITIALIZED $'(string, int) p\n(string &s, int &n) = &p'
check_rejected MATCH_CASE $'(int, int) initial = (1, 2)\n(int, int) | (int, string) v = initial\nmatch v\n    (int a, _)\n        print(a)'
check_rejected MATCH_CASE $'(int, string) | none v = (1, "x")\nmatch v\n    (int &a, string &b)\n        print(a)\n    none\n        print("none")'
check_rejected DISCARD $'(int a, _) = (1, 2)'
for name in values loops match keys; do
    cp "$TMP/$name.qui" "$TMP/$name.formatted.qui"
    "$QUIDRA" fmt "$TMP/$name.formatted.qui"
    [[ "$("$QUIDRA" "$TMP/$name.qui")" == "$("$QUIDRA" "$TMP/$name.formatted.qui")" ]]
done
