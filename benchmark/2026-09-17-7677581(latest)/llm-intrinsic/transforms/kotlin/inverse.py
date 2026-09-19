#!/usr/bin/env python3
"""I1 inverse transformer (kotlin, seed 20260918): anonymized source -> real source.

Text in, text out.  Applied to WHOLE WORD TOKENS ONLY.  A WORD token equal to a
mapped pseudo-word becomes the real token of that role; any other WORD token is
treated as an ordinary identifier and is passed through untouched (methodology 10
section 6.1).  String literals, character literals, numeric literals, back-quoted
identifiers and comments are never entered in either direction.

Only inverse-mapped source is ever given to the real toolchain (section 6.5).

Two ways a submission can make its own inverse image unbuildable, both of which
are MODEL failures (section 6.1, H-INVENT) and both of which are DETECTED here so
they are never mistaken for an infrastructure defect:

  ambiguity_report()  a submission that names one of its own bindings with a
                      pseudo-word; the inverse image then uses a keyword as an
                      identifier and will not build -> INVERSE_AMBIGUOUS.
  template_report()   a submission that puts a pseudo-word inside an in-literal
                      embedding form (`${...}` inside a text literal).  Text
                      literals are never entered in either direction, so such a
                      pseudo-word survives the inverse untranslated.  The pack
                      does not document that form -- it documents `+` (P5) -- so
                      this is out-of-pack invention, but it is REPORTED rather
                      than silently mis-built.  This detector exists because this
                      language, unlike most of the ten, can hide code inside a
                      text literal.

Usage:  inverse.py [INPUT]            # stdin when INPUT is omitted
        inverse.py INPUT -o OUTPUT
        inverse.py INPUT --report     # print INVERSE_AMBIGUOUS / template diagnoses
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ktlex import tokenize, render  # noqa: E402

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
    """Pseudo-words used as the submission's OWN binding names.

    A pseudo-word standing immediately after a binding introducer, or immediately
    before `=`, is being used as an identifier.  Reported, never repaired."""
    if inv is None:
        inv = load_inverse_mapping()
    # the two binding introducers in their transformed spelling
    introducers = {p for p, real in inv.items() if real in ("val", "var")}
    toks = [t for t in tokenize(source)
            if t[0] not in ("WS", "NEWLINE", "COMMENT")]
    hits = []
    for idx, (kind, raw) in enumerate(toks):
        if kind != "WORD" or raw not in inv:
            continue
        prv = toks[idx - 1] if idx > 0 else ("", "")
        nxt = toks[idx + 1] if idx + 1 < len(toks) else ("", "")
        if prv[0] == "WORD" and prv[1] in introducers:
            hits.append((idx, raw, "pseudo-word in binding-name position"))
        elif nxt[0] == "OP" and nxt[1] == "=":
            hits.append((idx, raw, "pseudo-word in assignment-target position"))
    return hits


def template_report(source, inv=None):
    """Pseudo-words hidden inside an in-literal embedding form (`$name`, `${...}`).

    Text literals are never entered in either direction, so anything here survives
    the inverse untranslated and the inverse image will not build."""
    if inv is None:
        inv = load_inverse_mapping()
    hits = []
    for idx, (kind, raw) in enumerate(tokenize(source)):
        if kind != "STRING" or "$" not in raw:
            continue
        for m in re.finditer(r"\$\{([^}]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)", raw):
            body = m.group(1) if m.group(1) is not None else m.group(2)
            for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", body or ""):
                if w in inv:
                    hits.append((idx, w, "pseudo-word inside an in-literal embedding"))
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
        th = template_report(src)
        for h in th:
            sys.stderr.write("TEMPLATE_ESCAPE token#%d %s: %s\n" % h)
        sys.stderr.write("TEMPLATE_ESCAPE count=%d\n" % len(th))
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(res)
    else:
        sys.stdout.write(res)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
