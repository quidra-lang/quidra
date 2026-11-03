#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/counts.qui" <<'QUI'
int a := 0
for 0.0 <= real16 x <= 1.0 step 0.1
    a += 1
int b := 0
for 0.0 <= real16b x <= 1.0 step 0.1
    b += 1
int c := 0
for 0.0 <= real32 x <= 1.0 step 0.1
    c += 1
int d := 0
for 0.0 <= real64 x <= 1.0 step 0.1
    d += 1
int e := 0
for 0.0 <= real x <= 1.0 step 0.1
    e += 1
int f := 0
for 100000000.0 < real32 x <= 100000008.0 step 1.0
    f += 1
int g := 0
for 1.0 >= real x > 0.0 step -0.1
    g += 1
print("{a} {b} {c} {d} {e} {f} {g}{NL}")
real endpoint := 0.3
for 0.0 <= real x <= endpoint step 0.1
    endpoint = 0.0
    print("{x} {x + 0.1} {x * 2.0} {x < 0.2} {real32(x)}{NL}")
QUI
expected=$'11 10 10 10 11 8 10\n0.0 0.1 0.0 true 0.0\n0.1 0.2 0.2 true 0.1\n0.2 0.3 0.4 false 0.2\n0.3 0.4 0.6 false 0.3'
actual="$("$QUIDRA" "$TMP/counts.qui")"
[[ "$actual" == "$expected" ]] || { printf '%s\n' "$actual"; exit 1; }
cat > "$TMP/discard.qui" <<'QUI'
int[] values := [1, 2, 3]
int calls := 0
for _ = values[nat i]
    values = []
    calls += 1
    print("{i}")
print(" {calls}{NL}")
int repetitions := 0
for 0 <= _ < 184467440737095516160000000000000000000
    repetitions += 1
    break
int range := 7
int in := 9
print("{repetitions} {range} {in}{NL}")
real limit := 0.3
int real_count := 0
for 0.0 <= _ <= limit step 0.1
    real_count += 1
print("{real_count}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/discard.qui")" == $'012 3\n1 7 9\n4' ]]
reject() {
    printf '%s\n' "$1" > "$TMP/reject.qui"
    if "$QUIDRA" check "$TMP/reject.qui" --json > "$TMP/reject.json"; then exit 1; fi
    python3 - "$TMP/reject.json" "$2" <<'PY'
import json,sys
assert any(item['code']==sys.argv[2] for item in json.load(open(sys.argv[1]))['diagnostics'])
PY
}
reject $'for 0.0 <= real64 x < 1.0\n    print(x)' FOR_HEADER
reject $'for 0 <= int _ < 3\n    print(1)' DISCARD
reject $'int[] values := [1]\nfor & _ := &values[]\n    print(1)' DISCARD
reject $'for 0.0 <= _ < 1.0 step 0.1\n    print(1)' DISCARD
reject $'range(3)' UNKNOWN_NAME
reject $'int[] values := [1]\nfor x in values\n    print(x)' FOR_HEADER
reject $'int[] values := [1]\nfor &x in values\n    print(x)' PARSE_ERROR
