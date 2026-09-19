#!/usr/bin/env python3
"""
Round-trip validator for the I1 (keyword anonymization) artifacts.

    python3 validate.py REAL.qui ANON.qui EXPECTED_STDOUT.txt

Checks, all of which must pass:

  R0 FORWARD_MATCH   forward(REAL) is byte-identical to ANON
  R1 ROUNDTRIP       inverse(ANON) is byte-identical to REAL
  R4 NONTRIVIAL      ANON differs from REAL in at least one token
  R5 DOMAIN          every differing token is exactly one mapped pair; no user
                     identifier, literal, comment or operator differs
  GATE               ANON contains no real reserved word of the language in a
                     grammar position (methodology 10 section 4.2a)
  BUILD              inverse(ANON) builds with the frozen recipe, exit 0
  RUN                the built program exits 0 and its stdout is byte-identical
                     to EXPECTED_STDOUT

Every check reports PASS or FAIL; the process exits 1 if any check fails, so a
corrupted input is rejected rather than silently accepted.
"""

import os
import subprocess
import sys
import tempfile

from qlex import lex, load_mapping
from forward import forward
from inverse import inverse

HERE = os.path.dirname(os.path.abspath(__file__))
QUIDRA = os.environ.get("QUIDRA_BIN", "/Users/koba/Desktop/Quidra/quidra/build/quidra")

# Every reserved word this language's lexer recognizes, plus the word-shaped type
# and unit designations. The conformance gate rejects any of these appearing as a
# grammar word token in anonymized source.
REAL_RESERVED = {
    "class", "override", "import", "super", "const", "return", "if", "elif",
    "else", "while", "for", "in", "match", "try", "break", "continue", "true",
    "false", "not", "and", "or", "int", "bool", "string", "void", "none",
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
    "float", "float32", "float64",
}
# Standard-vocabulary names that I1 leaves untransformed BY DESIGN (methodology
# 10 section 7.1): they are not reserved words and the pack documents them in
# their real spelling, so the gate must not flag them.
UNTRANSFORMED_BY_DESIGN = {"print", "write", "range", "len", "input"}

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("%-16s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))
    return ok


def token_view(text):
    return [(t.kind, t.text) for t in lex(text)]


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    real_path, anon_path, expected_path = argv
    real = open(real_path, encoding="utf-8").read()
    anon = open(anon_path, encoding="utf-8").read()
    expected = open(expected_path, encoding="utf-8").read()
    doc, fwd, inv = load_mapping()

    produced = forward(real, fwd)
    check("R0_FORWARD", produced == anon,
          "" if produced == anon else "forward(REAL) != ANON")

    back = inverse(anon, inv)
    check("R1_ROUNDTRIP", back == real,
          "" if back == real else "inverse(ANON) != REAL (first diff at byte %d)"
          % next((i for i, (a, b) in enumerate(zip(back, real)) if a != b), min(len(back), len(real))))

    check("R4_NONTRIVIAL", anon != real,
          "" if anon != real else "forward was a no-op")

    rt, at = token_view(real), token_view(anon)
    if len(rt) != len(at):
        check("R5_DOMAIN", False, "token count differs: %d vs %d" % (len(rt), len(at)))
    else:
        bad = []
        for (rk, rv), (ak, av) in zip(rt, at):
            if rk != ak or rv != av:
                if not (rk == "WORD" and ak == "WORD" and fwd.get(rv) == av):
                    bad.append((rv, av))
        check("R5_DOMAIN", not bad, "" if not bad else "off-domain changes: %r" % bad[:5])

    toks = lex(anon)
    hits = []
    for idx, t in enumerate(toks):
        if t.kind != "WORD":
            continue
        if t.text in UNTRANSFORMED_BY_DESIGN:
            continue
        k = idx - 1
        while k >= 0 and toks[k].kind in ("WS", "NEWLINE", "COMMENT"):
            k -= 1
        if k >= 0 and toks[k].kind == "OP" and toks[k].text == ".":
            continue  # member name, not a grammar word token
        if t.text in REAL_RESERVED:
            hits.append(t.text)
    check("GATE", not hits, "" if not hits else "real reserved words present: %r" % sorted(set(hits)))

    tmpdir = tempfile.mkdtemp(prefix="i1val_")
    src = os.path.join(tmpdir, "solution.qui")
    binp = os.path.join(tmpdir, "solution")
    open(src, "w", encoding="utf-8").write(back)
    b = subprocess.run([QUIDRA, "build", src, "-o", binp],
                       capture_output=True, text=True)
    check("BUILD", b.returncode == 0,
          "" if b.returncode == 0 else "exit %d: %s" % (b.returncode, (b.stderr or b.stdout).strip().splitlines()[:1]))
    if b.returncode == 0:
        r = subprocess.run([binp], capture_output=True, text=True)
        check("RUN_EXIT", r.returncode == 0, "" if r.returncode == 0 else "exit %d" % r.returncode)
        check("RUN_STDOUT", r.stdout == expected,
              "" if r.stdout == expected else "stdout differs from %s" % os.path.basename(expected_path))
    else:
        check("RUN_EXIT", False, "not run: build failed")
        check("RUN_STDOUT", False, "not run: build failed")

    failed = [n for n, ok, _ in results if not ok]
    print("VERDICT: %s%s" % ("ACCEPT" if not failed else "REJECT",
                             "" if not failed else "  (failed: %s)" % ", ".join(failed)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
