#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/source" "$TMP/home"
cat > "$TMP/source/main.qui" <<'QUI'
int answer()
    return 42

void announce()
    print("function-value")
    print(NL)

void invoke(fn<void>() operation)
    operation()
QUI

expect_rejected() {
    local name="$1"
    set +e
    HOME="$TMP/home" "$QUIDRA" package install "$TMP/source" --name "$name" \
        >"$TMP/out" 2>"$TMP/err"
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        echo "package name unexpectedly accepted: $name" >&2
        exit 1
    fi
}

# Installed package targets are written as Quidra identifiers. Reject spellings
# that the parser cannot name and language keywords. "math" is intentionally
# available to the canonical Quidra Math package.
expect_rejected "bad-name"
expect_rejected "9lives"
expect_rejected "class"

HOME="$TMP/home" "$QUIDRA" package install "$TMP/source" --name _pkg9 >/dev/null
cat > "$TMP/use.qui" <<'QUI'
import package = _pkg9
print(package.answer())
print(NL)
package.invoke(package.announce)
QUI
[[ "$(HOME="$TMP/home" "$QUIDRA" "$TMP/use.qui")" == "$(printf '42\nfunction-value')" ]]

HOME="$TMP/home" "$QUIDRA" package install "$TMP/source" --name math >/dev/null
cat > "$TMP/use-math.qui" <<'QUI'
import math
print(math.answer())
print(NL)
QUI
[[ "$(HOME="$TMP/home" "$QUIDRA" "$TMP/use-math.qui")" == "42" ]]

# io is no longer a Core standard namespace; it is available to packages.
HOME="$TMP/home" "$QUIDRA" package install "$TMP/source" --name io >/dev/null
cat > "$TMP/use-io.qui" <<'QUI'
import io
print(io.answer())
print(NL)
QUI
[[ "$(HOME="$TMP/home" "$QUIDRA" "$TMP/use-io.qui")" == "42" ]]

echo "package name tests: ok"
