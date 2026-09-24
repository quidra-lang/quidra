#!/usr/bin/env python3
"""Frozen generators for the Quidra adversarial sources that are produced, not hand-written.

ADV-21 (parser nesting) and the two ADV-22 mutations are defined byte-for-byte by
benchmark/template/methodology-assets/language_quality/adversarial_cases.json
(frozen_generators). This script regenerates them from the case-set rules, and
tests/benchmark_programs.sh verifies that the committed files are exactly what
it produces.
"""
from __future__ import annotations

from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
PROLOGUE = 'print("ADV-START")\nio.flush()\n'
EPILOGUE = 'io.flush()\nprint("ADV-END")\nio.flush()\n'


def nesting(depth: int) -> str:
    # No newline may be inserted inside the parenthesis runs.
    expression = "(" * depth + "1" + ")" * depth
    return PROLOGUE + f"int value = {expression}\n" + 'print("OBS=V:{value}")\n' + EPILOGUE


def mutations(valid: bytes) -> tuple[bytes, bytes]:
    length = len(valid)
    truncated = valid[: int(0.60 * length)]
    offset = length // 2
    injected = valid[:offset] + b"@#$" + valid[offset:]
    return truncated, injected


def midpoint_precondition(valid: bytes) -> None:
    """The valid file must contain no string literal or comment spanning its midpoint byte."""
    offset = len(valid) // 2
    line_start = valid.rfind(b"\n", 0, offset) + 1
    line = valid[line_start: valid.find(b"\n", offset)]
    column = offset - line_start
    if line.count(b'"', 0, column) % 2 == 1:
        raise SystemExit("ADV-22-valid.qui: the midpoint byte lies inside a string literal")
    if b"//" in line[:column]:
        raise SystemExit("ADV-22-valid.qui: the midpoint byte lies inside a comment")


def expected() -> dict[str, bytes]:
    valid = (HERE / "ADV-22-valid.qui").read_bytes()
    midpoint_precondition(valid)
    truncated, injected = mutations(valid)
    return {
        "ADV-21.qui": nesting(100000).encode("utf-8"),
        "ADV-21_depth1000.qui": nesting(1000).encode("utf-8"),
        "ADV-21_depth10000.qui": nesting(10000).encode("utf-8"),
        "ADV-22a.qui": truncated,
        "ADV-22b.qui": injected,
    }


def main() -> int:
    check = "--check" in sys.argv
    problems = []
    for name, data in expected().items():
        path = HERE / name
        if check:
            if not path.is_file() or path.read_bytes() != data:
                problems.append(name)
        else:
            path.write_bytes(data)
    if check and problems:
        print("generated adversarial sources are stale: " + ", ".join(problems), file=sys.stderr)
        return 1
    print("ok" if check else "generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
