#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PKG="$TMP/sample"
mkdir -p "$PKG/native" "$PKG/compiler"
cat >"$PKG/main.qui" <<'QUI'
int identity(int value)
    return value
QUI
cat >"$PKG/project.toml" <<'TOML'
[package]
name = "quidra-sample"
import = "sample"
display_name = "Sample"
version = "1.2.3"
repository = "https://github.com/example/sample"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1
numerics = ">=0.5.0 <0.6.0"

[assets]
linux-x86_64 = "sample-linux.tar.xz"

[native.source]
cpu = "native/cpu.cpp"

[native.pkg]
codec = "libcodec"

[compiler.extension]
graph = "compiler/graph.toml"
TOML
cat >"$PKG/native/cpu.cpp" <<'CPP'
extern "C" int sample_native_identity(int value) { return value; }
CPP
cat >"$PKG/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"
TOML

"$QUIDRA" package sync "$PKG"
cat >"$TMP/expected" <<'MANIFEST'
name = sample
version = 1.2.3
repository = https://github.com/example/sample
asset.linux-x86_64 = https://github.com/example/sample/releases/download/v1.2.3/sample-linux.tar.xz
native.source.cpu = native/cpu.cpp
native.pkg.codec = libcodec
requires.quidra = >=0.5.0 <0.6.0
requires.numerics = >=0.5.0 <0.6.0
MANIFEST
cmp "$TMP/expected" "$PKG/quidra.package"
"$QUIDRA" package sync "$PKG" --check
"$QUIDRA" package validate "$PKG"

printf '\n# drift\n' >>"$PKG/quidra.package"
if "$QUIDRA" package validate "$PKG" >/dev/null 2>&1; then
    echo "package validate accepted drift" >&2
    exit 1
fi
if "$QUIDRA" package sync "$PKG" --check >/dev/null 2>&1; then
    echo "package sync --check accepted drift" >&2
    exit 1
fi
"$QUIDRA" package sync "$PKG"
cmp "$TMP/expected" "$PKG/quidra.package"
