#!/usr/bin/env python3
"""
I1 FORWARD transformer for language_id = "swift".   text in -> text out.

    real source  --forward-->  anonymized source

Rules implemented (methodology 10):
  * section 6.2 step 1  lex the source with the language's real profile (lex10.py)
  * section 6.2 step 2  drop COMMENT tokens (fixtures only; model output has no
                        forward pass)
  * section 6.2 step 3  replace each WORD token whose text is a bound role token
                        with its pseudo-word -- WHOLE TOKEN MATCHES ONLY.  Because
                        substitution is driven by the token stream, a match can
                        never occur inside a text literal, a number or a comment,
                        and can never match a substring of a longer identifier.
                        It also cannot occur inside an embedded expression
                        (`\\(...)`), because the lexer keeps the whole literal --
                        embedding included -- in one STRING token (section 1
                        consequence 2; section 3.2 V18).
  * section 5.5         emission rule: a substituted token keeps the whitespace
                        that was already around it; where there was none, a single
                        space is inserted.  No space is inserted against a NEWLINE.
                        Every inserted character is recorded in the position map so
                        that the inverse can delete exactly those insertions and
                        section 6.4 R1 stays a plain byte-identity test.

Usage:
    forward.py IN.swift [-o OUT.swift] [--map OUT.posmap.json] [--keep-comments]
    forward.py < IN.swift > OUT.swift        (stdin/stdout; no position map)
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lex10  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LANGUAGE_ID = "swift"


def load_mapping(path=None):
    path = path or os.path.join(HERE, "mapping.json")
    with open(path, "r", encoding="utf-8") as fh:
        m = json.load(fh)
    fwd = {}
    for rk, entry in m["mapping"].items():
        fwd[entry["real"]] = entry["pseudo"]
    return m, fwd


def forward(src, fwd_map, keep_comments=False):
    """Return (transformed_text, position_map)."""
    toks = lex10.lex(src)

    if not keep_comments:
        toks = [t for t in toks if t.kind != "COMMENT"]

    pieces = []
    insertions = []  # offsets, in the OUTPUT text, of characters this pass inserted
    changed = []     # (real, pseudo) in order, for the R5 domain-containment check

    def out_len():
        return sum(len(p) for p in pieces)

    for idx, t in enumerate(toks):
        if t.kind == "WORD" and t.text in fwd_map:
            prev = toks[idx - 1] if idx > 0 else None
            nxt = toks[idx + 1] if idx + 1 < len(toks) else None

            # leading side
            need_before = not (prev is None or prev.kind in ("WS", "NEWLINE"))
            if need_before:
                insertions.append(out_len())
                pieces.append(" ")

            pieces.append(fwd_map[t.text])
            changed.append((t.text, fwd_map[t.text]))

            # trailing side
            need_after = not (nxt is None or nxt.kind in ("WS", "NEWLINE"))
            if need_after:
                insertions.append(out_len())
                pieces.append(" ")
        else:
            pieces.append(t.text)

    text = "".join(pieces)
    posmap = {
        "language_id": LANGUAGE_ID,
        "condition": "I1",
        "inserted_offsets": insertions,
        "comments_dropped": not keep_comments,
        "changed_tokens": changed,
    }
    return text, posmap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--map", dest="mapout")
    ap.add_argument("--mapping", dest="mapping")
    ap.add_argument("--keep-comments", action="store_true")
    args = ap.parse_args()

    src = (
        open(args.infile, "r", encoding="utf-8").read()
        if args.infile
        else sys.stdin.read()
    )
    _, fwd_map = load_mapping(args.mapping)
    text, posmap = forward(src, fwd_map, args.keep_comments)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        mapout = args.mapout or (args.out + ".posmap.json")
        with open(mapout, "w", encoding="utf-8") as fh:
            json.dump(posmap, fh, indent=2)
            fh.write("\n")
        sys.stderr.write(
            "forward: %d tokens substituted, %d whitespace characters inserted\n"
            % (len(posmap["changed_tokens"]), len(posmap["inserted_offsets"]))
        )
    else:
        sys.stdout.write(text)
        if args.mapout:
            with open(args.mapout, "w", encoding="utf-8") as fh:
                json.dump(posmap, fh, indent=2)
                fh.write("\n")


if __name__ == "__main__":
    main()
