#!/usr/bin/env python3
"""
I1 forward transformer: real source -> anonymized source.

  python3 forward.py < real.qui > anon.qui
  python3 forward.py real.qui anon.qui

Replaces whole WORD tokens that are grammar-significant reserved words with the
seed-20260918 pseudo-words of mapping.json. Text literals, comments, numbers,
operators, indentation and ordinary user identifiers are untouched. A WORD that
directly follows '.' is a member name, not a grammar word token, and is left
alone.
"""

import sys

from qlex import load_mapping, substitute


def forward(text, fwd=None):
    if fwd is None:
        _doc, fwd, _inv = load_mapping()
    out, changed, inserted = substitute(text, fwd, skip_after_dot=True)
    assert inserted == 0
    return out


def main(argv):
    if len(argv) >= 2:
        with open(argv[0], "r", encoding="utf-8") as fh:
            text = fh.read()
        out = forward(text)
        with open(argv[1], "w", encoding="utf-8") as fh:
            fh.write(out)
    else:
        sys.stdout.write(forward(sys.stdin.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
