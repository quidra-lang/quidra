#!/usr/bin/env python3
"""Check every Quidra code fence and execute examples with // output: expectations."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

QUIDRA = Path(sys.argv[1]).resolve()
ROOT = Path(sys.argv[2]).resolve()
DOCS = [
    ROOT / "README.md",
    ROOT / "docs/spec/language.md",
    ROOT / "docs/spec/llm-guide.md",
]

POINT = """class Point
    float x
    float y

"""
CONFIG = """class Config
    int retries = 3
    float timeout = 5.0
    string endpoint

"""
GENERIC = """class Box<T>
    T value

T first<T>(T[] values)
    return values[0]

class Convert
    T identity<T>(T value)
        return value

"""
LOOKUP = """int | none | error lookup(int id)
    if id < 0
        return error("invalid id")
    if id == 0
        return none
    return id

"""
PARENTS = """class Parent
    int value

class Child : Parent
    int extra

"""


def prelude(path: Path, code: str) -> str:
    rel = path.relative_to(ROOT).as_posix()
    if rel == "README.md":
        if code.lstrip().startswith("int | none | error doubled("):
            return LOOKUP
        if code.strip() == "Parent | Child value":
            return PARENTS
    if rel == "docs/spec/language.md":
        stripped = code.lstrip()
        if stripped.startswith("Box<int> box ="):
            return GENERIC
        if stripped.startswith("Point complete ="):
            return POINT + CONFIG
        if stripped.startswith("Point a =") or "Point point = Point(" in code:
            return POINT
    return ""


def fixtures(directory: Path) -> None:
    (directory / "geometry.qui").write_text(
        "class Point\n    int x\n    int y\n", encoding="utf-8"
    )
    (directory / "local.qui").write_text(
        "int local_value()\n    return 1\n", encoding="utf-8"
    )
    (directory / "shared.qui").write_text(
        "int shared_value()\n    return 1\n", encoding="utf-8"
    )
    shared = directory / "shared"
    shared.mkdir()
    (shared / "root.qui").write_text(
        "int root_value()\n    return 1\n", encoding="utf-8"
    )


def main() -> int:
    total = 0
    failures: list[str] = []
    fence = re.compile(r"\x60\x60\x60quidra\n(.*?)\x60\x60\x60", re.S)

    with tempfile.TemporaryDirectory(prefix="quidra-docs-") as raw_tmp:
        tmp = Path(raw_tmp)
        fixtures(tmp)

        for document in DOCS:
            text = document.read_text(encoding="utf-8")
            for index, code in enumerate(fence.findall(text)):
                total += 1
                source = tmp / f"{document.stem}-{index}.qui"
                source.write_text(prelude(document, code) + code, encoding="utf-8")
                result = subprocess.run(
                    [str(QUIDRA), "check", str(source)],
                    cwd=tmp,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                expected_failure = "// compile-time error" in code
                passed = result.returncode != 0 if expected_failure else result.returncode == 0
                if not passed:
                    mode = "expected rejection" if expected_failure else "expected acceptance"
                    details = (result.stdout + result.stderr).strip()
                    failures.append(
                        f"{document.relative_to(ROOT)} fence #{index}: {mode}; "
                        f"exit code {result.returncode}\n{details}"
                    )
                    continue

                expected_output = re.findall(r"// output:\s?(.*)$", code, re.M)
                if expected_output and not expected_failure:
                    native = subprocess.run(
                        [str(QUIDRA), "run", str(source)],
                        cwd=tmp,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=15,
                    )
                    actual = native.stdout.rstrip("\n")
                    expected = "\n".join(expected_output)
                    if native.returncode != 0 or actual != expected:
                        details = (native.stdout + native.stderr).strip()
                        failures.append(
                            f"{document.relative_to(ROOT)} fence #{index}: "
                            f"native output mismatch; exit code {native.returncode}; "
                            f"expected {expected!r}, got {actual!r}\n{details}"
                        )

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        print(
            f"documentation examples: {len(failures)} failure(s) out of {total}",
            file=sys.stderr,
        )
        return 1

    print(f"documentation examples: {total} fences verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
