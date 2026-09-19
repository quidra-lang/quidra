#!/usr/bin/env bash
set -euo pipefail

REPO="quidra-lang/quidra"
VERSION="${QUIDRA_VERSION:-latest}"
ARCH="$(uname -m)"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "quidra installer: Linux is required" >&2
  exit 1
fi

case "$ARCH" in
  x86_64|amd64) ASSET="quidra-linux-amd64.deb" ;;
  *)
    echo "quidra installer: unsupported architecture: $ARCH" >&2
    echo "Currently supported: x86_64 / amd64" >&2
    exit 1
    ;;
esac

if ! command -v apt-get >/dev/null 2>&1; then
  echo "quidra installer: apt-get is required by the Ubuntu installer" >&2
  exit 1
fi

if [[ "$(id -u)" -eq 0 ]]; then
  SUDO=()
elif command -v sudo >/dev/null 2>&1; then
  SUDO=(sudo)
else
  echo "quidra installer: root privileges or sudo are required" >&2
  exit 1
fi

if command -v curl >/dev/null 2>&1; then
  download() { curl -fL --retry 3 "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
  download() { wget -O "$2" "$1"; }
else
  echo "quidra installer: curl or wget is required" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ "$VERSION" == "latest" ]]; then
  URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
else
  URL="https://github.com/${REPO}/releases/download/v${VERSION}/${ASSET}"
fi

PACKAGE="$TMP_DIR/$ASSET"
echo "Downloading Quidra ${VERSION}..."
download "$URL" "$PACKAGE"

echo "Installing Quidra and native backend dependencies..."
"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y "$PACKAGE"

echo "Installed: $(quidra --version)"
echo "Run a program with: quidra your_program.qui"
