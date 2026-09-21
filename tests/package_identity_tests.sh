#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/package" "$TMP/home" "$TMP/project"
VERSION="$("$QUIDRA" --version | awk '{print $2}')"
IFS=. read -r MAJOR MINOR PATCH <<< "$VERSION"
NEXT_MINOR="$MAJOR.$((MINOR + 1)).0"

cat > "$TMP/package/main.qui" <<'QUI'
int answer()
    return 42
QUI
cat > "$TMP/package/quidra.package" <<EOF
name = named_pkg
version = 0.2.0
repository = https://github.com/example/named_pkg
requires.quidra = >=$VERSION <$NEXT_MINOR
EOF
cat > "$TMP/package/project.toml" <<EOF
[package]
name = "quidra-named"
import = "named_pkg"
display_name = "Quidra Named"
version = "0.2.0"
repository = "https://github.com/example/named_pkg"

[requires]
quidra = ">=$VERSION <$NEXT_MINOR"
abi = 1
EOF

HOME="$TMP/home" "$QUIDRA" package install "$TMP/package" >/dev/null
[[ -f "$TMP/home/.quidra/packages/named_pkg/main.qui" ]]
[[ "$(HOME="$TMP/home" "$QUIDRA" package list)" == "quidra-named 0.2.0 (import named_pkg)" ]]
HOME="$TMP/home" "$QUIDRA" package-info quidra-named --json > "$TMP/info.json"
python3 - "$TMP/info.json" <<'PY'
import json, sys
info=json.load(open(sys.argv[1]))
assert info["name"]=="quidra-named"
assert info["import"]=="named_pkg"
assert info["display_name"]=="Quidra Named"
assert info["path"].replace("\\","/").endswith("/.quidra/packages/named_pkg")
PY

cat > "$TMP/project/main.qui" <<'QUI'
import package = named_pkg
print(package.answer())
QUI
(
  cd "$TMP/project"
  HOME="$TMP/home" "$QUIDRA" package lock main.qui >/dev/null
  head -n1 quidra.lock | grep -qx 'quidra-lock-v3'
  grep -Eq '^quidra-named named_pkg 0\.2\.0 [0-9a-f]{64}$' quidra.lock
  HOME="$TMP/home" "$QUIDRA" package lock main.qui --check
  [[ "$(HOME="$TMP/home" "$QUIDRA" main.qui)" == "42" ]]
)
HOME="$TMP/home" "$QUIDRA" package remove quidra-named >/dev/null
[[ -z "$(HOME="$TMP/home" "$QUIDRA" package list)" ]]
echo "package identity tests: ok"
