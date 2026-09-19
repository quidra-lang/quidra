#!/usr/bin/env python3
"""
Round-trip / conformance validator for the I1 C++ column.

Three independent checks, each of which can both PASS and REJECT:

  roundtrip <real.cpp>   R1 byte identity: inverse(forward(F)) == F, and R5 domain
                         containment: every token the forward pass changed is a
                         mapped role token.
  gate <anon.cpp>        §4.2a conformance gate on a raw pre-inverse submission:
                           H_REAL   - a word token is a real reserved word of the
                                      mapped domain (the submission ignored the
                                      transformation);
                           H_INVENT - a word token has the pseudo-word shape
                                      (six lowercase letters, consonant/vowel
                                      alternating) but is not in the mapping.
  build <anon.cpp> <expected.txt>
                         inverse -> clang++ -std=c++20 -O2 solution.cpp -o solution ->
                         ./solution ; stdout must be byte-identical to <expected.txt>,
                         exit status 0, stderr empty.

  all <anon.cpp> <expected.txt>    gate + build.

Exit status 0 = accepted, 1 = rejected. Every rejection names the reason.
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from lex_cpp import lex  # noqa: E402
from forward import forward  # noqa: E402
from inverse import inverse, detect_ambiguous  # noqa: E402

CONS = set("bdfgklmnprstvz")
VOW = set("aeiou")

BUILD = ["clang++", "-std=c++20", "-O2"]


def load_mapping():
    with open(os.path.join(HERE, "mapping.json"), "r", encoding="ascii") as f:
        return json.load(f)


def is_pseudo_shape(w):
    if len(w) != 6:
        return False
    return all((c in CONS) if i % 2 == 0 else (c in VOW) for i, c in enumerate(w))


# ------------------------------------------------------------------ roundtrip


def check_roundtrip(path):
    m = load_mapping()
    src = open(path, "r", encoding="utf-8").read()
    anon = forward(src, m)
    back = inverse(anon, m)
    problems = []
    if back != src:
        problems.append("R1 FAILED: inverse(forward(F)) is not byte-identical to F")
    if anon == src:
        problems.append("R4 FAILED: forward(F) is a no-op")

    # R5 domain containment: diff the token streams
    ts, ta = lex(src), lex(anon)
    if len(ts) != len(ta):
        problems.append("R5 FAILED: token count changed (%d -> %d)" % (len(ts), len(ta)))
    else:
        fwd = m["forward"]
        for a, b in zip(ts, ta):
            if a.text == b.text:
                continue
            if a.kind != "WORD" or a.text not in fwd or fwd[a.text] != b.text:
                problems.append("R5 FAILED: out-of-domain change %r -> %r" % (a.text, b.text))
    return problems, anon


# ----------------------------------------------------------------------- gate


def check_gate(path):
    m = load_mapping()
    src = open(path, "r", encoding="utf-8").read()
    real_tokens = set(m["forward"].keys())
    pseudo = set(m["inverse"].keys())
    problems = []
    seen_real, seen_invent = [], []
    for t in lex(src):
        if t.kind != "WORD":
            continue
        if t.text in real_tokens:
            seen_real.append(t.text)
        elif t.text not in pseudo and is_pseudo_shape(t.text):
            seen_invent.append(t.text)
    if seen_real:
        problems.append("H_REAL: submission uses untransformed real reserved words: "
                        + ", ".join(sorted(set(seen_real))))
    if seen_invent:
        problems.append("H_INVENT: submission uses unmapped pseudo-word-shaped tokens: "
                        + ", ".join(sorted(set(seen_invent))))
    amb = detect_ambiguous(src, m)
    if amb:
        problems.append("INVERSE_AMBIGUOUS: mapped word used as a declared name: "
                        + ", ".join("%s(->%s)" % a for a in amb))
    return problems


# ---------------------------------------------------------------------- build


def check_build(path, expected_path, workdir=None):
    m = load_mapping()
    src = open(path, "r", encoding="utf-8").read()
    real = inverse(src, m)
    expected = open(expected_path, "rb").read()
    tmp = workdir or tempfile.mkdtemp(prefix="i1cpp_")
    cpp = os.path.join(tmp, "solution.cpp")
    binp = os.path.join(tmp, "solution")
    with open(cpp, "w", encoding="utf-8") as f:
        f.write(real)
    problems = []
    b = subprocess.run(BUILD + [cpp, "-o", binp], capture_output=True, text=True)
    if b.returncode != 0:
        problems.append("BUILD FAILED (exit %d): %s"
                        % (b.returncode, (b.stderr.strip().splitlines() or [""])[0]))
        return problems, tmp
    r = subprocess.run([binp], capture_output=True)
    if r.returncode != 0:
        problems.append("RUN FAILED: exit status %d" % r.returncode)
    if r.stdout != expected:
        problems.append("OUTPUT MISMATCH:\n  expected: %r\n  actual:   %r"
                        % (expected, r.stdout))
    if r.stderr:
        problems.append("STDERR NOT EMPTY: %r" % r.stderr)
    return problems, tmp


# ----------------------------------------------------------------------- main


def report(name, problems):
    if problems:
        print("REJECT  %s" % name)
        for p in problems:
            print("        - %s" % p)
        return 1
    print("ACCEPT  %s" % name)
    return 0


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mode, path = sys.argv[1], sys.argv[2]
    if mode == "roundtrip":
        problems, _ = check_roundtrip(path)
        sys.exit(report("roundtrip %s" % os.path.basename(path), problems))
    if mode == "gate":
        sys.exit(report("gate %s" % os.path.basename(path), check_gate(path)))
    if mode == "build":
        problems, _ = check_build(path, sys.argv[3])
        sys.exit(report("build %s" % os.path.basename(path), problems))
    if mode == "all":
        p = check_gate(path)
        b, _ = check_build(path, sys.argv[3])
        sys.exit(report("all %s" % os.path.basename(path), p + b))
    raise SystemExit("unknown mode " + mode)


if __name__ == "__main__":
    main()
