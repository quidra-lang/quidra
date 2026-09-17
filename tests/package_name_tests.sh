#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/source" "$TMP/home"
cat > "$TMP/source/main.qui" <<'QUI'
int answer()
    return 42
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
# that the parser cannot name, plus keywords and standard namespaces that cannot
# resolve as installed-package targets.
expect_rejected "bad-name"
expect_rejected "9lives"
expect_rejected "class"
expect_rejected "math"

HOME="$TMP/home" "$QUIDRA" package install "$TMP/source" --name _pkg9 >/dev/null
cat > "$TMP/use.qui" <<'QUI'
import package = _pkg9
print(package.answer())
QUI
[[ "$(HOME="$TMP/home" "$QUIDRA" "$TMP/use.qui")" == "42" ]]

echo "package name tests: ok"
