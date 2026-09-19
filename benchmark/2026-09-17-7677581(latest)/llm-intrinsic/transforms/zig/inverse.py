#!/usr/bin/env python3
"""
I1 inverse transformer for language_id = "zig".   Text in, text out.

  inverse(transformed_source [, position map]) -> real_source

Methodology 10 section 6.2: the same machinery run with the inverse mapping,
followed by deletion of exactly the whitespace insertions the position map
records (section 5.5).  With a position map the result is byte-identical to
F_nocomments (section 6.4, R1).  Without one -- the model-output path, which
never had a forward pass -- the substitution is applied and no whitespace is
deleted, which is correct because none was inserted.

Ambiguity handling (section 6.1, "scope honesty"): any WORD equal to a mapped
pseudo-word is treated as a role token.  A submission that NAMES A VARIABLE with
a pseudo-word therefore inverts to source that uses a keyword as an identifier.
--detect-ambiguous reports that condition so the harness can record the trial as
INVERSE_AMBIGUOUS (a model failure of type H-INVENT, not an infrastructure fault).

Usage:
  python3 inverse.py IN.zig -o OUT.zig --posmap IN.posmap.json
  python3 inverse.py IN.zig --detect-ambiguous
"""

import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lex10 = _load("lex10")


def load_mapping(path=None):
    path = path or os.path.join(HERE, "mapping.json")
    with open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    return doc, dict(doc["reverse"])


def inverse(text, rev_map, posmap=None):
    toks = lex10.lex(text)

    drop = set(posmap["inserted_ws_token_indices"]) if posmap else set()
    if posmap and posmap.get("transformed_token_count") not in (None, len(toks)):
        raise SystemExit(
            "POSMAP_MISMATCH: position map describes %d tokens, input lexes to %d"
            % (posmap["transformed_token_count"], len(toks)))

    pieces = []
    for i, t in enumerate(toks):
        if i in drop:
            if t.kind != "WS" or t.text != " ":
                raise SystemExit(
                    "POSMAP_MISMATCH: token %d is %r, not an inserted single space"
                    % (i, t.text))
            continue
        if t.kind == "WORD" and not t.builtin and t.text in rev_map:
            pieces.append(rev_map[t.text])
        else:
            pieces.append(t.text)
    return "".join(pieces)


def declared_identifiers(text, introducers=None):
    """The names a submission introduces: the WORD immediately following a binding
    or function introducer.  In transformed source the introducers are the
    pseudo-words bound to the value-binding and function roles (K02, K03, K01);
    a name drawn from the pseudo-word set is what makes the inverse ambiguous."""
    if introducers is None:
        _, rev = load_mapping()
        introducers = set(p for p, real in rev.items() if real in ("var", "const", "fn"))
        introducers |= {"var", "const", "fn"}
    toks = [t for t in lex10.lex(text) if t.kind not in ("WS", "NEWLINE", "COMMENT")]
    names = []
    for i, t in enumerate(toks[:-1]):
        if t.kind == "WORD" and not t.builtin and t.text in introducers:
            nxt = toks[i + 1]
            if nxt.kind == "WORD" and not nxt.builtin:
                names.append(nxt.text)
    return names


def ambiguous_names(text, mapping_path=None):
    """Names the submission invents that collide with a pseudo-word.

    The entry-point name is excluded: K22 binds the toolchain-fixed entry-point
    name, so its pseudo-word appearing after the function introducer is the pack's
    own documented spelling, not an invented identifier.
    """
    doc, rev = load_mapping(mapping_path)
    entry_pw = doc["mapping"].get("K22", {}).get("pseudo")
    documented = {entry_pw} if entry_pw else set()
    return sorted(set(n for n in declared_identifiers(text)
                      if n in rev and n not in documented))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--posmap")
    ap.add_argument("--mapping")
    ap.add_argument("--detect-ambiguous", action="store_true")
    args = ap.parse_args()

    with open(args.input, "r", encoding="utf-8") as fh:
        text = fh.read()

    doc, rev = load_mapping(args.mapping)

    if args.detect_ambiguous:
        clash = ambiguous_names(text, args.mapping)
        if clash:
            print("INVERSE_AMBIGUOUS %s" % " ".join(clash))
            return 2
        print("INVERSE_UNAMBIGUOUS")
        return 0

    posmap = None
    if args.posmap:
        with open(args.posmap, "r", encoding="utf-8") as fh:
            posmap = json.load(fh)

    out = inverse(text, rev, posmap)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(out)
    else:
        sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
