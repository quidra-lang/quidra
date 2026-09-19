#!/usr/bin/env python3
"""
Pre-flight check (b): harness-convention sufficiency.

Feeds a simulated raw model submission - written ONLY from the Reference Pack,
in pseudo-words, containing every element the pack documents and omitting only
the conventions the pack withholds (entry file name, build command, output
binary name) - through the real pipeline:

    raw output -> code extraction -> conformance gate -> inverse -> fixups
               -> build with the frozen recipe -> run -> oracle

Pass criterion: the submission passes, and the only fixup applied is the
unstated-convention one (H1: write the source to the entry file name). Zero
pack-documented fixups.

Usage: python3 harness_sim.py raw_submission.md expected_output.txt
"""

import os
import re
import subprocess
import sys
import tempfile

from qlex import lex
from inverse import inverse
from validate import REAL_RESERVED, UNTRANSFORMED_BY_DESIGN

HERE = os.path.dirname(os.path.abspath(__file__))
QUIDRA = os.environ.get("QUIDRA_BIN", "/Users/koba/Desktop/Quidra/quidra/build/quidra")

# The two conventions the prompt withholds because stating them would identify
# the toolchain. The harness supplies both; the trial is never asked for them.
ENTRY_FILENAME = "solution.qui"
BUILD_RECIPE = [QUIDRA, "build", ENTRY_FILENAME, "-o", "solution"]

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("%-24s %s%s" % (name, "PASS" if cond else "FAIL", ("  " + detail) if detail else ""))


def extract(raw):
    """Frozen extraction rule: content of the LAST fenced block, tag line ignored."""
    blocks = re.findall(r"```[^\n]*\n(.*?)```", raw, re.S)
    if not blocks:
        return raw
    return blocks[-1]


def gate(src):
    toks = lex(src)
    hits = []
    for idx, t in enumerate(toks):
        if t.kind != "WORD" or t.text in UNTRANSFORMED_BY_DESIGN:
            continue
        k = idx - 1
        while k >= 0 and toks[k].kind in ("WS", "NEWLINE", "COMMENT"):
            k -= 1
        if k >= 0 and toks[k].kind == "OP" and toks[k].text == ".":
            continue
        if t.text in REAL_RESERVED:
            hits.append(t.text)
    return sorted(set(hits))


def main(argv):
    raw_path, expected_path = argv
    raw = open(raw_path, encoding="utf-8").read()
    expected = open(expected_path, encoding="utf-8").read()

    code = extract(raw)
    check("EXTRACTION", code.strip() != "" and "```" not in code,
          "%d bytes from the last fenced block" % len(code))

    hits = gate(code)
    check("CONFORMANCE_GATE", not hits, "" if not hits else "real reserved words: %r" % hits)

    real = inverse(code)

    tmp = tempfile.mkdtemp(prefix="harness_")
    # fixup H1 (unstated convention): the harness names the file. No other fixup
    # is applied: the pack documents the entry-point shape (top-level statements),
    # needs no unit declaration, needs no module load, needs no failure marker.
    src = os.path.join(tmp, ENTRY_FILENAME)
    open(src, "w", encoding="utf-8").write(real)
    fixups_unstated = ["H1 entry file name"]
    fixups_documented = []
    check("FIXUPS_DOCUMENTED", not fixups_documented, "must be zero; applied %r" % fixups_documented)
    print("%-24s %s" % ("FIXUPS_UNSTATED", fixups_unstated))

    b = subprocess.run([QUIDRA, "build", ENTRY_FILENAME, "-o", "solution"],
                       cwd=tmp, capture_output=True, text=True)
    check("BUILD", b.returncode == 0,
          "recipe: quidra build %s -o solution -> exit %d" % (ENTRY_FILENAME, b.returncode))
    if b.returncode != 0:
        sys.stdout.write(b.stderr)
        return 1
    r = subprocess.run([os.path.join(tmp, "solution")], capture_output=True, text=True)
    check("RUN_EXIT_ZERO", r.returncode == 0, "exit %d" % r.returncode)
    check("STDERR_EMPTY", r.stderr == "", repr(r.stderr[:60]))
    check("STDOUT_EXACT", r.stdout == expected,
          "" if r.stdout == expected else "differs from the oracle")
    print("--- produced ---")
    sys.stdout.write(r.stdout)

    print("HARNESS SIM:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
