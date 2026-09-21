#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

QUIDRA_VERSION="$("$QUIDRA" --version | awk '{print $2}')"
IFS=. read -r QMAJOR QMINOR QPATCH <<< "$QUIDRA_VERSION"
NEXT_MINOR="$QMAJOR.$((QMINOR + 1)).0"

REPO="$TMP/sample_pkg"
HOME_DIR="$TMP/home"
ASSET_ROOT="$TMP/assets/v0.1.0"
ASSET_PAYLOAD="$TMP/asset-payload"
mkdir -p "$REPO" "$HOME_DIR" "$ASSET_ROOT" "$ASSET_PAYLOAD/nvidia/lib"
printf 'managed asset fixture\n' > "$ASSET_PAYLOAD/nvidia/marker.txt"
printf 'runtime\n' > "$ASSET_PAYLOAD/nvidia/lib/libfixture.so"
tar -C "$ASSET_PAYLOAD" -czf "$ASSET_ROOT/sample-assets.tar.gz" nvidia

git -C "$REPO" init -q -b main
git -C "$REPO" config user.email "quidra-tests@example.invalid"
git -C "$REPO" config user.name "Quidra Tests"

cat > "$REPO/main.qui" <<'QUI'
int answer()
    return 42
QUI

cat > "$REPO/quidra.package" <<EOF_MANIFEST
name = sample_pkg
version = 0.1.0
repository = file://$REPO
description = Sample package for release tests
license = MIT
homepage = https://example.invalid/sample_pkg
asset.default = file://$ASSET_ROOT/sample-assets.tar.gz
requires.quidra = >=$QUIDRA_VERSION <$NEXT_MINOR
EOF_MANIFEST

git -C "$REPO" add main.qui quidra.package
git -C "$REPO" commit -q -m 'release 0.1.0'
git -C "$REPO" tag v0.1.0

cat > "$REPO/quidra.package" <<EOF_MANIFEST
name = sample_pkg
version = 0.2.0
repository = file://$REPO
requires.quidra = >=99.0.0 <100.0.0
EOF_MANIFEST

git -C "$REPO" add quidra.package
git -C "$REPO" commit -q -m 'release 0.2.0 incompatible'
git -C "$REPO" tag v0.2.0

# Untagged work must never be selected by the installer.
cat > "$REPO/quidra.package" <<EOF_MANIFEST
name = sample_pkg
version = 0.3.0
repository = file://$REPO
requires.quidra = >=$QUIDRA_VERSION <$NEXT_MINOR
EOF_MANIFEST

git -C "$REPO" add quidra.package
git -C "$REPO" commit -q -m 'unreleased development'

QUIDRA_ALLOW_FILE_PACKAGE_ASSETS=1 HOME="$HOME_DIR" "$QUIDRA" install "file://$REPO" >/dev/null
[[ "$(HOME="$HOME_DIR" "$QUIDRA" list)" == "sample_pkg 0.1.0" ]]
grep -q '^version = 0.1.0$' "$HOME_DIR/.quidra/packages/sample_pkg/quidra.package"
grep -q '^managed asset fixture$' "$HOME_DIR/.quidra/packages/sample_pkg/nvidia/marker.txt"

HOME="$HOME_DIR" "$QUIDRA" package-info sample_pkg --json > "$TMP/package-info.json"
python3 - "$TMP/package-info.json" <<'PY'
import json
import sys
info = json.load(open(sys.argv[1]))
assert info["name"] == "sample_pkg"
assert info["version"] == "0.1.0"
assert info["description"] == "Sample package for release tests"
assert info["license"] == "MIT"
assert info["homepage"] == "https://example.invalid/sample_pkg"
assert info["assets"]["default"].endswith("/sample-assets.tar.gz")
assert info["requirements"]["quidra"]
assert info["path"].replace("\\", "/").endswith("/.quidra/packages/sample_pkg")
PY

set +e
HOME="$HOME_DIR" "$QUIDRA" install "file://$REPO@0.2.0" >"$TMP/out" 2>"$TMP/err"
rc=$?
set -e

[[ "$rc" -eq 1 ]]
grep -q 'requires Quidra' "$TMP/err"

QUIDRA_ALLOW_FILE_PACKAGE_ASSETS=1 HOME="$HOME_DIR" "$QUIDRA" install "file://$REPO@0.1.0" >/dev/null
[[ "$(HOME="$HOME_DIR" "$QUIDRA" list)" == "sample_pkg 0.1.0" ]]
grep -q '^managed asset fixture$' "$HOME_DIR/.quidra/packages/sample_pkg/nvidia/marker.txt"

mkdir -p "$TMP/deps/base_pkg" "$TMP/deps/app_pkg"

cat > "$TMP/deps/base_pkg/main.qui" <<'QUI'
int value()
    return 7
QUI

cat > "$TMP/deps/base_pkg/quidra.package" <<EOF_MANIFEST
name = base_pkg
version = 0.1.0
requires.quidra = >=$QUIDRA_VERSION <$NEXT_MINOR
EOF_MANIFEST

cat > "$TMP/deps/app_pkg/main.qui" <<'QUI'
import base = base_pkg

int answer()
    return base.value()
QUI

cat > "$TMP/deps/app_pkg/quidra.package" <<EOF_MANIFEST
name = app_pkg
version = 0.1.0
requires.quidra = >=$QUIDRA_VERSION <$NEXT_MINOR
requires.base_pkg = >=0.2.0 <0.3.0
EOF_MANIFEST

cat > "$TMP/dependency-use.qui" <<'QUI'
import app = app_pkg
print(app.answer())
QUI

set +e
QUIDRA_PACKAGE_PATH="$TMP/deps" "$QUIDRA" check "$TMP/dependency-use.qui" >"$TMP/dependency.out" 2>"$TMP/dependency.err"
dependency_rc=$?
set -e

[[ "$dependency_rc" -eq 1 ]]
grep -q 'PACKAGE_DEPENDENCY' "$TMP/dependency.err"
grep -q 'requires package' "$TMP/dependency.err"

sed -i.bak 's/version = 0.1.0/version = 0.2.0/' "$TMP/deps/base_pkg/quidra.package"
rm -f "$TMP/deps/base_pkg/quidra.package.bak"

QUIDRA_PACKAGE_PATH="$TMP/deps" "$QUIDRA" check "$TMP/dependency-use.qui" >/dev/null

echo "package release tests: ok"
