#!/usr/bin/env python3
"""
The real trial pipeline for the I1 C++ column, end to end
(methodology 10 §6.5, §9.2 extraction, §9.3 fixups, §4.2 oracle).

    extraction -> §4.2a gate -> inverse -> harness fixups -> build -> run -> oracle

Usage:
    python3 pipeline.py RAW_MODEL_OUTPUT.txt EXPECTED.txt [WORKDIR]

Harness conventions supplied here, never asked of the model (§9.1):
    H1  entry filename            solution.cpp
    H6  build command             clang++ -std=c++20 -O2 solution.cpp -o solution
        run command               ./solution
No other fixup is applied: everything the Reference Pack documents (the two module
directives, the entry point, the result statement) must come from the submission
itself (§9.3 "no fixup may supply anything the pack documents").
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from inverse import inverse  # noqa: E402
from validate import check_gate  # noqa: E402

ENTRY_FILENAME = "solution.cpp"
BUILD_CMD = ["clang++", "-std=c++20", "-O2", "solution.cpp", "-o", "solution"]
RUN_CMD = ["./solution"]


def extract(raw):
    """§9.2: content of the LAST fenced block, else the whole output."""
    if "```" not in raw:
        return raw
    parts = raw.split("```")
    # fenced bodies are the odd-indexed parts
    bodies = [parts[i] for i in range(1, len(parts), 2)]
    if not bodies:
        return raw
    body = bodies[-1]
    lines = body.split("\n")
    if lines and lines[0].strip() and " " not in lines[0].strip():
        lines = lines[1:]  # drop a language tag line
    out = "\n".join(lines)
    return out if out.endswith("\n") else out + "\n"


def run(raw_path, expected_path, workdir=None):
    raw = open(raw_path, "r", encoding="utf-8").read()
    expected = open(expected_path, "rb").read()
    work = workdir or tempfile.mkdtemp(prefix="i1cpp_pipe_")
    os.makedirs(work, exist_ok=True)

    steps = []
    submission = extract(raw)
    sub_path = os.path.join(work, "submission_anon.txt")
    open(sub_path, "w", encoding="utf-8").write(submission)
    steps.append(("extract", "OK", "%d bytes" % len(submission)))

    gate = check_gate(sub_path)
    steps.append(("gate", "FAIL" if gate else "OK", "; ".join(gate) or "no H_REAL, no H_INVENT"))
    if gate:
        return False, steps, work

    real = inverse(submission)
    # H1: the harness, not the model, supplies the entry filename
    open(os.path.join(work, ENTRY_FILENAME), "w", encoding="utf-8").write(real)
    steps.append(("fixup H1", "OK", "written as " + ENTRY_FILENAME))
    steps.append(("fixups pack-documented", "OK", "0 applied"))

    b = subprocess.run(BUILD_CMD, cwd=work, capture_output=True, text=True)
    steps.append(("build", "OK" if b.returncode == 0 else "FAIL",
                  " ".join(BUILD_CMD) + " -> exit %d" % b.returncode))
    if b.returncode != 0:
        steps.append(("build stderr", "FAIL", b.stderr.strip()[:400]))
        return False, steps, work

    r = subprocess.run(RUN_CMD, cwd=work, capture_output=True, timeout=10)
    steps.append(("run", "OK" if r.returncode == 0 else "FAIL", "exit %d" % r.returncode))
    ok_out = r.stdout == expected
    steps.append(("oracle stdout", "OK" if ok_out else "FAIL",
                  "byte-identical" if ok_out else "expected %r got %r" % (expected, r.stdout)))
    ok_err = not r.stderr
    steps.append(("oracle stderr", "OK" if ok_err else "FAIL",
                  "empty" if ok_err else repr(r.stderr)))
    passed = r.returncode == 0 and ok_out and ok_err
    return passed, steps, work


def main():
    passed, steps, work = run(sys.argv[1], sys.argv[2],
                              sys.argv[3] if len(sys.argv) > 3 else None)
    for name, status, detail in steps:
        print("  %-24s %-4s %s" % (name, status, detail))
    print("  PIPELINE VERDICT: %s   (workdir %s)" % ("PASS" if passed else "FAIL", work))
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
