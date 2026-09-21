#!/usr/bin/env python3
"""Assert the image's installed toolchains equal the declared pins.

Running this at image build time turns `toolchains.json` from documentation into
an enforced contract: if an upstream repository serves a different build than the
pin names, the image fails to build instead of quietly shipping an unrecorded
toolchain into a measurement run.

The observed fingerprints are also written into the image so a finished run can
state exactly what it measured against.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys

MANIFEST = Path(__file__).resolve().parent / "toolchains.json"
OBSERVED = Path("/opt/quidra-benchmark/toolchains-observed.json")

# Each entry: language -> (argv, pin key, regex capturing the comparable version)
CHECKS: dict[str, tuple[list[str], str, str]] = {
    "Python": (["python3", "--version"], "PYTHON_PIN", r"Python (\d+\.\d+\.\d+)"),
    "C++": (["c++", "--version"], "CLANG_PIN", r"clang version (\d+\.\d+\.\d+)"),
    "CMake": (["cmake", "--version"], "CMAKE_PIN", r"cmake version (\d+\.\d+\.\d+)"),
    "Rust": (["rustc", "--version"], "RUST_PIN", r"rustc (\d+\.\d+\.\d+)"),
    "Go": (["go", "version"], "GO_PIN", r"go(\d+\.\d+\.\d+)"),
    # Java omits trailing zeros, so a .0 release reports "javac 26" rather than
    # "javac 26.0.0". Accept both spellings and let the pin comparison decide.
    "Java": (["javac", "-version"], "JAVA_PIN", r"javac (\d+(?:\.\d+\.\d+)?)"),
    "Kotlin": (["kotlinc", "-version"], "KOTLIN_PIN", r"kotlinc-jvm (\d+\.\d+\.\d+)"),
    "Node": (["node", "--version"], "NODE_PIN", r"v(\d+\.\d+\.\d+)"),
    "TypeScript": (["tsc", "--version"], "TYPESCRIPT_PIN", r"Version (\d+\.\d+\.\d+)"),
    "Swift": (["swift", "--version"], "SWIFT_PIN", r"Swift version (\d+\.\d+(?:\.\d+)?)"),
    "Zig": (["zig", "version"], "ZIG_PIN", r"(\d+\.\d+\.\d+)"),
}


def capture(argv: list[str]) -> str:
    completed = subprocess.run(
        argv,
        shell=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    if completed.returncode != 0:
        raise SystemExit(
            f"toolchain check failed ({' '.join(argv)}): "
            f"{(completed.stderr or completed.stdout).strip()}"
        )
    return (completed.stdout + "\n" + completed.stderr).strip()


def main() -> int:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    pins = manifest["toolchains"]
    problems: list[str] = []
    observed: dict[str, str] = {}

    for language, (argv, pin_key, pattern) in CHECKS.items():
        try:
            output = capture(argv)
        except SystemExit as exc:
            problems.append(str(exc))
            continue
        observed[language] = output.splitlines()[0]
        match = re.search(pattern, output)
        if not match:
            problems.append(f"{language}: could not parse a version from {output!r}")
            continue
        expected = pins[pin_key]
        if match.group(1) != expected:
            problems.append(
                f"{language}: image installed {match.group(1)} but toolchains.json pins "
                f"{expected}. Update the pin deliberately; do not silently ship a "
                "different toolchain into a measurement run."
            )

    if problems:
        print("runtime image toolchain verification failed:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 2

    OBSERVED.parent.mkdir(parents=True, exist_ok=True)
    OBSERVED.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "image": manifest["image"],
                "pins": pins,
                "observed": observed,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"ok": True, "observed": observed}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
