#!/usr/bin/env python3
"""
I1 FORWARD transformer for the C++ column: real source -> anonymized source.

Text in, text out. Whole-token matches only: a word is rewritten only when the
lexer classified it as a WORD token, so nothing inside a string literal, a
character literal, a number or a comment is ever touched, and no substring of an
identifier is ever touched (methodology 10 §1 consequences 2-4, §6.2 step 3).

Emission rule (§5.5): a pseudo-word is never placed immediately next to another
word character. For this language that is guaranteed without inserting any
whitespace, because maximal-munch lexing means a WORD token can never be adjacent
to another identifier character; the transformer asserts this and refuses to emit
if it were ever violated. The unit position map is therefore empty and the inverse
has nothing to delete, which is what makes the §6.4 R1 round-trip a plain
byte-identity test.

Usage:
    python3 forward.py IN.cpp            # writes to stdout
    python3 forward.py IN.cpp OUT.cpp
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lex_cpp import lex, render, ID_CONT  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def load_mapping(path=None):
    with open(path or os.path.join(HERE, "mapping.json"), "r", encoding="ascii") as f:
        return json.load(f)


def forward(source, mapping=None):
    """Real source text -> anonymized source text."""
    m = mapping or load_mapping()
    fwd = m["forward"]
    toks = lex(source)
    assert render(toks) == source, "lexer lost bytes"

    out = []
    changed = 0
    inserted_ws = 0
    for idx, t in enumerate(toks):
        if t.kind != "WORD" or t.text not in fwd:
            out.append(t.text)
            continue
        pw = fwd[t.text]
        # emission-rule guard: never let the pseudo-word merge with a neighbour
        prev_char = out[-1][-1] if out and out[-1] else ""
        nxt_char = ""
        for u in toks[idx + 1:]:
            if u.text:
                nxt_char = u.text[0]
                break
        if prev_char in ID_CONT:
            out.append(" ")
            inserted_ws += 1
        out.append(pw)
        if nxt_char in ID_CONT:
            out.append(" ")
            inserted_ws += 1
        changed += 1

    if inserted_ws:
        raise SystemExit(
            "EMISSION_RULE: %d whitespace characters would have to be inserted; "
            "this column's round-trip is specified as insertion-free" % inserted_ws
        )
    forward.last_changed = changed
    return "".join(out)


forward.last_changed = 0


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: forward.py IN.cpp [OUT.cpp]")
    src = open(sys.argv[1], "r", encoding="utf-8").read()
    res = forward(src)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", encoding="utf-8") as f:
            f.write(res)
        sys.stderr.write("forward: %d tokens rewritten -> %s\n"
                         % (forward.last_changed, sys.argv[2]))
    else:
        sys.stdout.write(res)


if __name__ == "__main__":
    main()
