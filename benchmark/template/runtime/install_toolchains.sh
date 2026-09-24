#!/usr/bin/env bash
# Install the nine pinned comparison-language toolchains.
#
# Quidra is deliberately not installed here. It is built during the run from the
# evaluated snapshot at /quidra-benchmark/repo, so baking a compiler into the
# image would measure the wrong commit.
set -euo pipefail

: "${RUST_PIN:?}" "${GO_PIN:?}" "${JAVA_PIN:?}" "${JAVA_BUILD:?}" "${KOTLIN_PIN:?}"
: "${NODE_PIN:?}" "${TYPESCRIPT_PIN:?}" "${SWIFT_PIN:?}" "${ZIG_PIN:?}"

ARCH="$(dpkg --print-architecture)"
case "$ARCH" in
  amd64) GNU_ARCH=x86_64; GO_ARCH=amd64; NODE_ARCH=x64; JDK_ARCH=x64; SWIFT_PLATFORM=ubuntu24.04 ;;
  arm64) GNU_ARCH=aarch64; GO_ARCH=arm64; NODE_ARCH=arm64; JDK_ARCH=aarch64; SWIFT_PLATFORM=ubuntu24.04-aarch64 ;;
  *) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

fetch() {
  curl --fail --location --silent --show-error --retry 3 --retry-delay 2 "$1" -o "$2"
}

# Some projects have changed their release asset naming between versions. Trying
# the known spellings keeps a pinned version installable without pretending a
# single URL template is eternal.
fetch_any() {
  local dest="$1"; shift
  local url
  for url in "$@"; do
    if curl --fail --location --silent --show-error --retry 2 "$url" -o "$dest"; then
      echo "fetched: $url" >&2
      return 0
    fi
  done
  echo "none of the candidate URLs for $dest could be fetched: $*" >&2
  return 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> Rust ${RUST_PIN}"
fetch https://static.rust-lang.org/rustup/dist/${GNU_ARCH}-unknown-linux-gnu/rustup-init "$WORK/rustup-init"
chmod +x "$WORK/rustup-init"
RUSTUP_HOME=/opt/rust CARGO_HOME=/opt/rust "$WORK/rustup-init" \
  -y --no-modify-path --profile minimal --default-toolchain "$RUST_PIN"
ln -sf /opt/rust/bin/rustc /usr/local/bin/rustc
ln -sf /opt/rust/bin/cargo /usr/local/bin/cargo

echo "==> Go ${GO_PIN}"
fetch "https://go.dev/dl/go${GO_PIN}.linux-${GO_ARCH}.tar.gz" "$WORK/go.tgz"
tar -C /opt -xzf "$WORK/go.tgz"
ln -sf /opt/go/bin/go /usr/local/bin/go
ln -sf /opt/go/bin/gofmt /usr/local/bin/gofmt

echo "==> Java ${JAVA_PIN}+${JAVA_BUILD} (Temurin)"
# Deliberately not the Adoptium apt repository: it already carries revisions
# newer than this pin, and apt installs the newest, so the repository cannot
# express a reproducible pin. The exact-build endpoint can.
fetch "https://api.adoptium.net/v3/binary/version/jdk-${JAVA_PIN}%2B${JAVA_BUILD}/linux/${JDK_ARCH}/jdk/hotspot/normal/eclipse" \
  "$WORK/jdk.tar.gz"
mkdir -p /opt/java
tar -C /opt/java --strip-components=1 -xzf "$WORK/jdk.tar.gz"
for tool in java javac jar jlink jpackage; do
  [ -x "/opt/java/bin/$tool" ] && ln -sf "/opt/java/bin/$tool" "/usr/local/bin/$tool"
done

echo "==> Kotlin ${KOTLIN_PIN}"
fetch "https://github.com/JetBrains/kotlin/releases/download/v${KOTLIN_PIN}/kotlin-compiler-${KOTLIN_PIN}.zip" \
  "$WORK/kotlin.zip"
unzip -q "$WORK/kotlin.zip" -d /opt
ln -sf /opt/kotlinc/bin/kotlinc /usr/local/bin/kotlinc
ln -sf /opt/kotlinc/bin/kotlin /usr/local/bin/kotlin

echo "==> Node ${NODE_PIN} and TypeScript ${TYPESCRIPT_PIN}"
fetch "https://nodejs.org/dist/v${NODE_PIN}/node-v${NODE_PIN}-linux-${NODE_ARCH}.tar.xz" "$WORK/node.txz"
mkdir -p /opt/node
tar -C /opt/node --strip-components=1 -xJf "$WORK/node.txz"
ln -sf /opt/node/bin/node /usr/local/bin/node
ln -sf /opt/node/bin/npm /usr/local/bin/npm
/opt/node/bin/npm install --global --no-fund --no-audit "typescript@${TYPESCRIPT_PIN}"
ln -sf /opt/node/bin/tsc /usr/local/bin/tsc

echo "==> Swift ${SWIFT_PIN}"
fetch_any "$WORK/swift.tar.gz" \
  "https://download.swift.org/swift-${SWIFT_PIN}-release/${SWIFT_PLATFORM//./}/swift-${SWIFT_PIN}-RELEASE/swift-${SWIFT_PIN}-RELEASE-${SWIFT_PLATFORM}.tar.gz" \
  "https://download.swift.org/swift-${SWIFT_PIN}-release/$(echo "$SWIFT_PLATFORM" | tr -d '.')/swift-${SWIFT_PIN}-RELEASE/swift-${SWIFT_PIN}-RELEASE-${SWIFT_PLATFORM}.tar.gz"
mkdir -p /opt/swift
tar -C /opt/swift --strip-components=1 -xzf "$WORK/swift.tar.gz"
ln -sf /opt/swift/usr/bin/swift /usr/local/bin/swift
ln -sf /opt/swift/usr/bin/swiftc /usr/local/bin/swiftc

echo "==> Zig ${ZIG_PIN}"
fetch_any "$WORK/zig.tar.xz" \
  "https://ziglang.org/download/${ZIG_PIN}/zig-${GNU_ARCH}-linux-${ZIG_PIN}.tar.xz" \
  "https://ziglang.org/download/${ZIG_PIN}/zig-linux-${GNU_ARCH}-${ZIG_PIN}.tar.xz"
mkdir -p /opt/zig
tar -C /opt/zig --strip-components=1 -xJf "$WORK/zig.tar.xz"
ln -sf /opt/zig/zig /usr/local/bin/zig

rm -rf /var/lib/apt/lists/*
