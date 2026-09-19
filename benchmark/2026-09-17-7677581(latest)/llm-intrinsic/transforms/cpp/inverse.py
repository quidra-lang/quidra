#!/usr/bin/env python3
"""
I1 INVERSE transformer for the C++ column: anonymized source -> real source.

Text in, text out. Whole-token matches only, same lexer as forward.py, so string
literals, character literals, numbers and comments pass through byte for byte.

Semantics on model output (methodology 10 §6.1 "scope honesty"): any WORD equal to
a mapped pseudo-word is treated as a role token and mapped back; any other WORD is
treated as an identifier and left alone. A submission that names a variable with a
pseudo-word therefore inverse-maps to a program that uses a keyword as an
identifier and fails to build; `detect_ambiguous()` reports that case so the
harness can record it as INVERSE_AMBIGUOUS / H_INVENT rather than as an
infrastructure defect.

Usage:
    python3 inverse.py IN.cpp            # writes to stdout
    python3 inverse.py IN.cpp OUT.cpp
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lex_cpp import lex, render  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def load_mapping(path=None):
    with open(path or os.path.join(HERE, "mapping.json"), "r", encoding="ascii") as f:
        return json.load(f)


def inverse(source, mapping=None):
    """Anonymized source text -> real source text."""
    m = mapping or load_mapping()
    inv = m["inverse"]
    toks = lex(source)
    assert render(toks) == source, "lexer lost bytes"

    out = []
    changed = 0
    for t in toks:
        if t.kind == "WORD" and t.text in inv:
            out.append(inv[t.text])
            changed += 1
        else:
            out.append(t.text)
    inverse.last_changed = changed
    return "".join(out)


inverse.last_changed = 0


def detect_ambiguous(source, mapping=None):
    """
    Return the list of pseudo-words used in a DECLARING position in the submission
    (immediately after another mapped type word, or immediately before '=' / '(' at
    statement level is not decidable lexically, so this is the conservative form):
    a pseudo-word that the inverse will turn into a keyword but that the submission
    appears to use as a name. Reported, never silently repaired.
    """
    m = mapping or load_mapping()
    inv = m["inverse"]
    toks = [t for t in lex(source) if t.kind not in ("WS", "NEWLINE", "COMMENT")]
    hits = []
    for i, t in enumerate(toks):
        if t.kind != "WORD" or t.text not in inv:
            continue
        real = inv[t.text]
        # a type/qualifier word followed by a mapped word used as the declared name
        if real in ("int", "long", "const") and i + 1 < len(toks):
            u = toks[i + 1]
            if u.kind == "WORD" and u.text in inv and inv[u.text] not in (
                "int", "long", "const", "main"
            ):
                hits.append((u.text, inv[u.text]))
    return hits


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: inverse.py IN.cpp [OUT.cpp]")
    src = open(sys.argv[1], "r", encoding="utf-8").read()
    res = inverse(src)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", encoding="utf-8") as f:
            f.write(res)
        sys.stderr.write("inverse: %d tokens restored -> %s\n"
                         % (inverse.last_changed, sys.argv[2]))
    else:
        sys.stdout.write(res)


if __name__ == "__main__":
    main()
