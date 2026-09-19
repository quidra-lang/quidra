#!/usr/bin/env python3
"""
I1 INVERSE transformer for language_id = "swift".   text in -> text out.

    anonymized source  --inverse-->  real source

This is the transformer of methodology 10 section 6.5 step 3: it is what turns a
model submission written in the anonymized surface into something the real
toolchain can build.  It runs on the same lex10 machinery with the inverse
mapping; STRING, NUMBER and COMMENT tokens are never touched, so a pseudo-word
that appears inside a text literal (or inside a `\\(...)` embedding, which the
lexer keeps within the STRING token) survives verbatim.

Two modes:

  MAP MODE (--map POSMAP.json)
      Deletes exactly the whitespace characters the forward pass recorded as
      insertions.  This makes  inverse(forward(F)) == F_nocomments  a plain
      byte-identity test -- the R1 invariant of section 6.4 -- with no
      canonicalization step, which section 6.4 forbids.

  NO-MAP MODE (default; the mode used on model output)
      A model submission never had a forward pass, so there is no position map and
      no byte-identity requirement: section 6.5 only requires that the inverse
      image be buildable and semantically the real program.  In this mode the
      inverse inserts and deletes NOTHING -- it only rewrites whole word tokens in
      place -- so it cannot disturb the submission's spacing.  That matters in this
      particular language, whose operator parser IS whitespace-sensitive (spacing
      decides prefix / infix / postfix position): a transformer that "tidied"
      whitespace here could change the meaning of a program.  It does not, by
      construction.

Ambiguity handling (section 6.1, "scope honesty"): any WORD equal to a mapped
pseudo-word is treated as a role token; any other WORD is an identifier.  A model
that names a variable with a pseudo-word therefore yields an inverse image that
uses a keyword as an identifier and fails to build; --report-ambiguous flags that
case so the harness can record INVERSE_AMBIGUOUS.

Usage:
    inverse.py IN.anon.swift [-o OUT.swift] [--map IN.anon.swift.posmap.json]
    inverse.py < IN.anon.swift > OUT.swift
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lex10  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LANGUAGE_ID = "swift"

# Words that introduce a binding in this language.  A pseudo-word appearing
# immediately after one of them is being used as a NAME, not as a grammar word.
BINDERS = ("let", "var", "func", "struct", "class", "enum", "typealias")


def load_mapping(path=None):
    path = path or os.path.join(HERE, "mapping.json")
    with open(path, "r", encoding="utf-8") as fh:
        m = json.load(fh)
    inv = {}
    for rk, entry in m["mapping"].items():
        inv[entry["pseudo"]] = entry["real"]
    return m, inv


def inverse(text, inv_map, posmap=None):
    """Return (real_source, restored_tokens)."""
    toks = lex10.lex(text)
    pieces = []
    restored = []
    for t in toks:
        if t.kind == "WORD" and t.text in inv_map:
            pieces.append(inv_map[t.text])
            restored.append((t.text, inv_map[t.text]))
        else:
            pieces.append(t.text)
    out = "".join(pieces)

    if posmap is not None:
        # Delete exactly the characters the forward pass inserted.  Offsets index
        # the TRANSFORMED text; substitution is not length-preserving in general,
        # so the deletion is done by walking the token stream rather than by
        # assuming offsets stay put after substitution.
        out = _delete_insertions(text, toks, inv_map, posmap["inserted_offsets"])

    return out, restored


def _delete_insertions(text, toks, inv_map, inserted_offsets):
    """Rebuild the output while dropping the recorded inserted characters."""
    drop = set(inserted_offsets)
    pieces = []
    for t in toks:
        if t.kind == "WORD" and t.text in inv_map:
            pieces.append(inv_map[t.text])
        elif t.kind == "WS":
            kept = "".join(
                ch for k, ch in enumerate(t.text) if (t.pos + k) not in drop
            )
            pieces.append(kept)
        else:
            pieces.append(t.text)
    return "".join(pieces)


def find_ambiguous(text, inv_map):
    """Pseudo-words used where only a user identifier can stand.

    Cheap lexical approximation, enough for the harness's INVERSE_AMBIGUOUS flag:
    a pseudo-word that directly follows a binding introducer is being declared as
    a name, and its inverse image would be a keyword in a name position.
    """
    toks = [t for t in lex10.lex(text)
            if t.kind not in ("WS", "NEWLINE", "COMMENT")]
    hits = []
    for i, t in enumerate(toks):
        if t.kind != "WORD" or t.text not in inv_map:
            continue
        prev = toks[i - 1] if i > 0 else None
        if prev is not None and prev.kind == "WORD":
            prev_real = inv_map.get(prev.text, prev.text)
            if prev_real in BINDERS:
                hits.append((t.pos, t.text,
                             "declared as a name after '%s'" % prev.text))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--map", dest="mapin")
    ap.add_argument("--mapping", dest="mapping")
    ap.add_argument("--report-ambiguous", action="store_true")
    args = ap.parse_args()

    text = (
        open(args.infile, "r", encoding="utf-8").read()
        if args.infile
        else sys.stdin.read()
    )
    _, inv_map = load_mapping(args.mapping)

    posmap = None
    mapin = args.mapin
    if mapin is None and args.infile:
        candidate = args.infile + ".posmap.json"
        if os.path.exists(candidate):
            mapin = candidate
    if mapin:
        with open(mapin, "r", encoding="utf-8") as fh:
            posmap = json.load(fh)

    if args.report_ambiguous:
        for pos, w, why in find_ambiguous(text, inv_map):
            sys.stderr.write("INVERSE_AMBIGUOUS at %d: %s %s\n" % (pos, w, why))

    out, restored = inverse(text, inv_map, posmap)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(out)
        sys.stderr.write(
            "inverse: %d tokens restored, position map %s\n"
            % (len(restored), "used" if posmap else "absent (no-map mode)")
        )
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
