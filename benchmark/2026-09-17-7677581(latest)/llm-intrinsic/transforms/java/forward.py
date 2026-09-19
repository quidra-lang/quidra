#!/usr/bin/env python3
"""I1 forward transformer (java, seed 20260918): real source -> anonymized source.

Text in, text out.  Substitution is applied to WHOLE WORD TOKENS ONLY.  String
literals, character literals, numeric literals and comments are never entered
(methodology 10, section 1 consequence 2).  Ordinary user identifiers are never
renamed: only the tokens listed in mapping.json's "mapping" are replaced.

Emission rule (methodology 10 section 5.5): a WORD token produced by the scanner
is maximal, so replacing its text with another word token can never merge it with
an adjacent token.  No whitespace is inserted, and the unit position map is
therefore empty -- which is what makes the round trip a plain byte-identity test
rather than one weakened by a canonicalization step.

Usage:  forward.py [INPUT]            # stdin when INPUT is omitted
        forward.py INPUT -o OUTPUT
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from javalex import tokenize, render  # noqa: E402

MAPPING_PATH = os.path.join(HERE, "mapping.json")


def load_mapping():
    with open(MAPPING_PATH, encoding="ascii") as f:
        doc = json.load(f)
    return doc["mapping"]


def forward(source, mapping=None):
    if mapping is None:
        mapping = load_mapping()
    toks = tokenize(source)
    out = []
    changed = 0
    for kind, raw in toks:
        if kind == "WORD" and raw in mapping:
            out.append(("WORD", mapping[raw]))
            changed += 1
        else:
            out.append((kind, raw))
    result = render(out)

    # Emission self-check: the transformed text must re-tokenize to the same
    # token shape, i.e. every substituted word is still exactly one WORD token
    # in the same position.  This is what proves no token merged.
    back = tokenize(result)
    if [k for k, _ in back] != [k for k, _ in out]:
        raise SystemExit("forward: emission self-check failed (token shape changed)")
    forward.last_changed = changed
    return result


forward.last_changed = 0


def main(argv):
    out_path = None
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "-o":
            out_path = argv[i + 1]
            i += 2
        else:
            args.append(argv[i])
            i += 1
    src = open(args[0], encoding="utf-8").read() if args else sys.stdin.read()
    res = forward(src)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(res)
    else:
        sys.stdout.write(res)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
