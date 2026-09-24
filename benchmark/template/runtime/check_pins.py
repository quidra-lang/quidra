#!/usr/bin/env python3
"""Check that every pinned toolchain artifact still exists upstream.

A full image build takes tens of minutes and fails late when an upstream
release moves or a URL template changes. This does the same reachability
check in seconds, for every architecture the image claims to support, so pin
rot is a fast, obvious failure instead of a slow, confusing one.

It deliberately only checks existence. Whether the artifact reports the pinned
version is verify_toolchains.py's job, and only a real build can answer it.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import urllib.error
import urllib.request

MANIFEST = Path(__file__).resolve().parent / "toolchains.json"

ARCHITECTURES = {
    "amd64": {"gnu": "x86_64", "go": "amd64", "node": "x64", "jdk": "x64",
              "swift": "ubuntu24.04"},
    "arm64": {"gnu": "aarch64", "go": "arm64", "node": "arm64", "jdk": "aarch64",
              "swift": "ubuntu24.04-aarch64"},
}


def pinned_urls(pins: dict[str, str], arch: dict[str, str]) -> dict[str, list[str]]:
    """Each entry maps a label to the candidate URLs the installer would try."""
    swift_dir = arch["swift"].replace(".", "")
    return {
        "Rust (rustup)": [
            f"https://static.rust-lang.org/rustup/dist/{arch['gnu']}-unknown-linux-gnu/rustup-init"
        ],
        "Rust (toolchain)": [
            f"https://static.rust-lang.org/dist/rust-{pins['RUST_PIN']}-"
            f"{arch['gnu']}-unknown-linux-gnu.tar.gz"
        ],
        "Go": [f"https://go.dev/dl/go{pins['GO_PIN']}.linux-{arch['go']}.tar.gz"],
        "Java": [
            f"https://api.adoptium.net/v3/binary/version/"
            f"jdk-{pins['JAVA_PIN']}%2B{pins['JAVA_BUILD']}/linux/{arch['jdk']}"
            f"/jdk/hotspot/normal/eclipse"
        ],
        "Kotlin": [
            f"https://github.com/JetBrains/kotlin/releases/download/"
            f"v{pins['KOTLIN_PIN']}/kotlin-compiler-{pins['KOTLIN_PIN']}.zip"
        ],
        "Node": [
            f"https://nodejs.org/dist/v{pins['NODE_PIN']}/"
            f"node-v{pins['NODE_PIN']}-linux-{arch['node']}.tar.xz"
        ],
        "TypeScript": [
            f"https://registry.npmjs.org/typescript/{pins['TYPESCRIPT_PIN']}"
        ],
        "Swift": [
            f"https://download.swift.org/swift-{pins['SWIFT_PIN']}-release/{swift_dir}/"
            f"swift-{pins['SWIFT_PIN']}-RELEASE/"
            f"swift-{pins['SWIFT_PIN']}-RELEASE-{arch['swift']}.tar.gz"
        ],
        # The installer tries both spellings because the project renamed its
        # assets between releases; one of them has to resolve.
        "Zig": [
            f"https://ziglang.org/download/{pins['ZIG_PIN']}/"
            f"zig-{arch['gnu']}-linux-{pins['ZIG_PIN']}.tar.xz",
            f"https://ziglang.org/download/{pins['ZIG_PIN']}/"
            f"zig-linux-{arch['gnu']}-{pins['ZIG_PIN']}.tar.xz",
        ],
    }


# Some release CDNs reject the default Python-urllib agent outright, so the
# check would report a perfectly healthy pin as missing.
USER_AGENT = "quidra-benchmark-pin-check"


def reachable(url: str, timeout: float) -> tuple[bool, str]:
    # A ranged GET rather than HEAD: some release hosts answer HEAD with 403
    # while serving the artifact perfectly well.
    request = urllib.request.Request(
        url, headers={"Range": "bytes=0-0", "User-Agent": USER_AGENT}
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return 200 <= response.status < 300, str(response.status)
    except urllib.error.HTTPError as exc:
        return False, f"HTTP {exc.code}"
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        return False, f"unreachable: {exc}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=sorted(ARCHITECTURES), action="append")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    pins = manifest["toolchains"]
    architectures = args.arch or sorted(ARCHITECTURES)

    failures: list[str] = []
    results: dict[str, dict[str, str]] = {}
    for name in architectures:
        results[name] = {}
        for label, candidates in pinned_urls(pins, ARCHITECTURES[name]).items():
            statuses = []
            for url in candidates:
                ok, status = reachable(url, args.timeout)
                statuses.append(f"{status} {url}")
                if ok:
                    results[name][label] = url
                    break
            else:
                results[name][label] = "MISSING"
                failures.append(f"{name} / {label}: " + "; ".join(statuses))

    print(json.dumps({"pins": pins, "resolved": results}, indent=2, sort_keys=True))
    if failures:
        print("\npinned artifacts that no longer resolve:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(
            "\nUpdate the pin in runtime/toolchains.json deliberately and record the "
            "change; do not loosen the pin to whatever happens to be current.",
            file=sys.stderr,
        )
        return 2
    print(f"\nall pinned artifacts resolve for: {', '.join(architectures)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
