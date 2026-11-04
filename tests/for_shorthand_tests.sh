#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/accepted.qui" <<'QUI'
for: int8 i < 3
    print(i)
    print(NL)
for: 3 > nat8 i
    print(i)
    print(NL)
for: int8 i <= 2
    print(i)
    print(NL)
for: 2 >= nat8 i
    print(i)
    print(NL)
int i := 99
int n := 3
for i < n step 2
    print(i)
    print(NL)
print(i)
print(NL)
for n > i step -2
    print(i)
    print(NL)
print(i)
print(NL)
for: int8 empty < -1
    print(int(999))
    print(NL)
for: 0 > nat8 empty
    print(int(999))
    print(NL)
for: 0 <= int8 final < 2
    print(final)
    print(NL)
QUI
expected=$'0\n1\n2\n2\n1\n0\n0\n1\n2\n2\n1\n0\n0\n2\n2\n1\n1\n0\n1'
out="$("$QUIDRA" "$TMP/accepted.qui")"
[[ "$out" == "$expected" ]] || { printf '%s\n' "$out"; exit 1; }
for header in 'for i > 5' 'for 0 < i'; do
    printf 'int i := 0\n%s\n    print(i)\n    print(NL)\n' "$header" > "$TMP/reject.qui"
    if "$QUIDRA" check "$TMP/reject.qui" --json > "$TMP/reject.json"; then exit 1; fi
    python3 - "$TMP/reject.json" <<'PYJSON'
import json, sys
assert any(d['code'] == 'FOR_HEADER' for d in json.load(open(sys.argv[1]))['diagnostics'])
PYJSON
done
# Real-family omitted zero is typed after the variable is resolved. Dyadic
# steps make these expectations independent of decimal rounding.
cat > "$TMP/real.qui" <<'QUI'
for: real x <= 1.0 step 0.5
    print(x >= 0.0 and x <= 1.0)
    print(NL)
for 1.0 >= real x step -0.5
    print(x >= 0.0 and x <= 1.0)
    print(NL)
for: real64 x <= 1.0 step 0.5
    print(x >= 0.0 and x <= 1.0)
    print(NL)
for 1.0 >= real64 x step -0.5
    print(x >= 0.0 and x <= 1.0)
    print(NL)
real64 existing := 9.0
for existing <= -1.0 step 0.5
    print(int(999))
    print(NL)
print(existing == 9.0)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/real.qui")" == $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' ]]
for header in 'for: real x <= 1.0' 'for: 1.0 >= real64 x'; do
    printf '%s\n    print(x >= 0.0 and x <= 1.0)\n' "$header" > "$TMP/reject-real.qui"
    if "$QUIDRA" check "$TMP/reject-real.qui" --json > "$TMP/reject-real.json"; then exit 1; fi
    python3 - "$TMP/reject-real.json" <<'PYJSON'
import json, sys
assert any(d['code'] == 'FOR_HEADER' for d in json.load(open(sys.argv[1]))['diagnostics'])
PYJSON
done
# Formatting preserves the full and omitted-zero headers without migration.
cp "$TMP/accepted.qui" "$TMP/formatted.qui"
"$QUIDRA" fmt "$TMP/formatted.qui"
[[ "$("$QUIDRA" "$TMP/formatted.qui")" == "$expected" ]]

