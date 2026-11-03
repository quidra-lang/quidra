#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/bound.qui" <<'QUI'
int a := 1
int &r
&r = &a
r = 3
print("{r} {a}{NL}")
int &choice
if a == 3
    &choice = &a
else
    &choice = &a
print("{choice}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/bound.qui")" == $'3 3\n3' ]]
cat > "$TMP/maybe.qui" <<'QUI'
int a := 1
int &r
if a == 2
    &r = &a
print(r)
QUI
if "$QUIDRA" "$TMP/maybe.qui" > "$TMP/out" 2> "$TMP/err"; then exit 1; fi
grep -Fq UNBOUND_REFERENCE "$TMP/err"
cat > "$TMP/iteration.qui" <<'QUI'
int a := 0
for int i < 2
    int &r
    if i == 0
        &r = &a
    r = i
QUI
if "$QUIDRA" "$TMP/iteration.qui" > "$TMP/out" 2> "$TMP/err"; then exit 1; fi
grep -Fq UNBOUND_REFERENCE "$TMP/err"
for expression in r '&r' '&a == &r' 'r = 3'; do
    printf 'int a := 0\nint &r\n%s\n' "$expression" > "$TMP/never.qui"
    if "$QUIDRA" check "$TMP/never.qui" > "$TMP/out" 2> "$TMP/err"; then exit 1; fi
    grep -Fq UNBOUND_REFERENCE "$TMP/err"
done
