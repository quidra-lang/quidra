#!/usr/bin/env python3
"""
I1 inverse transformer: anonymized source -> real source.

  python3 inverse.py < anon.qui > real.qui
  python3 inverse.py anon.qui real.qui

Replaces whole WORD tokens that are seed-20260918 pseudo-words with the real
reserved word they stand for. Text literals, comments, numbers, operators,
indentation and ordinary user identifiers are untouched.

Asymmetry with forward.py, deliberate and recorded in preflight.md: the inverse
also maps a pseudo-word that follows '.', so a submission that spells a member
name with a pseudo-word still maps back to buildable real source. This cannot
break the round-trip invariant, because forward.py never emits a pseudo-word in
that position and no pseudo-word occurs anywhere in the task material
(mapping.json filter 7).
"""

import sys

from qlex import load_mapping, substitute


def inverse(text, inv=None):
    if inv is None:
        _doc, _fwd, inv = load_mapping()
    out, changed, inserted = substitute(text, inv, skip_after_dot=False)
    assert inserted == 0
    return out


def main(argv):
    if len(argv) >= 2:
        with open(argv[0], "r", encoding="utf-8") as fh:
            text = fh.read()
        out = inverse(text)
        with open(argv[1], "w", encoding="utf-8") as fh:
            fh.write(out)
    else:
        sys.stdout.write(inverse(sys.stdin.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
