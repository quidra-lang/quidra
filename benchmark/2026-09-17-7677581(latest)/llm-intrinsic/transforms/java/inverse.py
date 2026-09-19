#!/usr/bin/env python3
"""I1 inverse transformer (java, seed 20260918): anonymized source -> real source.

Text in, text out.  Applied to WHOLE WORD TOKENS ONLY.  A WORD token equal to a
mapped pseudo-word becomes the real token of that role; any other WORD token is
treated as an ordinary identifier and is passed through untouched (methodology 10
section 6.1).  String literals, character literals, numeric literals and comments
are never entered in either direction.

Only inverse-mapped source is ever given to the real toolchain (section 6.5).

A submission that names one of its own variables with a pseudo-word produces a
program whose inverse image uses a keyword as an identifier; that program will
fail to build and is reported here as INVERSE_AMBIGUOUS so the harness can record
it as a model failure rather than an infrastructure defect.

Usage:  inverse.py [INPUT]            # stdin when INPUT is omitted
        inverse.py INPUT -o OUTPUT
        inverse.py INPUT --report     # print an INVERSE_AMBIGUOUS diagnosis
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from javalex import tokenize, render  # noqa: E402

MAPPING_PATH = os.path.join(HERE, "mapping.json")


def load_inverse_mapping():
    with open(MAPPING_PATH, encoding="ascii") as f:
        doc = json.load(f)
    return doc["inverse_mapping"]


def inverse(source, inv=None):
    if inv is None:
        inv = load_inverse_mapping()
    toks = tokenize(source)
    out = []
    changed = 0
    for kind, raw in toks:
        if kind == "WORD" and raw in inv:
            out.append(("WORD", inv[raw]))
            changed += 1
        else:
            out.append((kind, raw))
    result = render(out)
    back = tokenize(result)
    if [k for k, _ in back] != [k for k, _ in out]:
        raise SystemExit("inverse: emission self-check failed (token shape changed)")
    inverse.last_changed = changed
    return result


inverse.last_changed = 0


def ambiguity_report(source, inv=None):
    """Count pseudo-words used in declaration position, i.e. as the submission's
    own identifier names.  A cheap lexical proxy: a pseudo-word immediately
    preceded by another pseudo-word that denotes a type, or followed by '=' .
    Reported, never repaired."""
    if inv is None:
        inv = load_inverse_mapping()
    toks = [t for t in tokenize(source) if t[0] not in ("WS", "NEWLINE", "COMMENT")]
    hits = []
    for idx, (kind, raw) in enumerate(toks):
        if kind == "WORD" and raw in inv:
            nxt = toks[idx + 1] if idx + 1 < len(toks) else ("", "")
            if nxt[0] == "OP" and nxt[1] == "=":
                hits.append((idx, raw, "pseudo-word in assignment-target position"))
    return hits


def main(argv):
    out_path = None
    report = False
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "-o":
            out_path = argv[i + 1]
            i += 2
        elif argv[i] == "--report":
            report = True
            i += 1
        else:
            args.append(argv[i])
            i += 1
    src = open(args[0], encoding="utf-8").read() if args else sys.stdin.read()
    res = inverse(src)
    if report:
        hits = ambiguity_report(src)
        for h in hits:
            sys.stderr.write("INVERSE_AMBIGUOUS token#%d %s: %s\n" % h)
        sys.stderr.write("INVERSE_AMBIGUOUS count=%d\n" % len(hits))
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(res)
    else:
        sys.stdout.write(res)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
