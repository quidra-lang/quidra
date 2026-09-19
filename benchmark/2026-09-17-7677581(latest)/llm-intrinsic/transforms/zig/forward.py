#!/usr/bin/env python3
"""
I1 forward transformer for language_id = "zig".   Text in, text out.

  forward(real_source) -> transformed_source  (+ position map)

Methodology 10 section 6.2, steps 1-3 and 6:
  1. lex the source with the real profile (lex10.py);
  2. drop COMMENT tokens (fixtures only; model output never has a forward pass);
  3. replace each WORD token whose text is a bound role token with its pseudo-word,
     applying the emission rule of section 5.5;
  6. re-emit, recording every inserted whitespace character in the position map.

Section 5.5 emission rule, as implemented:
  a substituted token keeps whatever whitespace already surrounded it; where there
  was none, exactly one space is inserted -- EXCEPT that nothing is inserted when
  the neighbouring token is NEWLINE (this language is not indentation-significant,
  so the INDENT_RUN half of the exception cannot arise).  Every inserted space is
  recorded, so section 6.4 R1 is a plain byte-identity test.

Never touched, structurally rather than by regex care: STRING, CHAR, NUMBER,
COMMENT tokens, and any WORD carrying the builtin guard G1 (a name after "@").

Usage:
  python3 forward.py IN.zig -o OUT.zig --posmap OUT.posmap.json
  python3 forward.py IN.zig                 # transformed source to stdout
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

NO_PAD_KINDS = ("NEWLINE",)   # section 5.5 exception; INDENT_RUN n/a for this language


def load_mapping(path=None):
    path = path or os.path.join(HERE, "mapping.json")
    with open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    fwd = {e["real"]: e["pseudo"] for e in doc["mapping"].values()}
    return doc, fwd


def strip_comments(src):
    """F_nocomments -- the exact text section 6.4 R1 compares against."""
    toks = lex10.lex(src)
    return "".join(t.text for t in toks if t.kind != "COMMENT")


def forward(src, fwd_map):
    toks = [t for t in lex10.lex(src) if t.kind != "COMMENT"]

    out = []            # list of (kind, text, inserted_bool, substituted_bool)
    n = len(toks)
    for idx, t in enumerate(toks):
        if t.kind == "WORD" and not t.builtin and t.text in fwd_map:
            prev = toks[idx - 1] if idx > 0 else None
            nxt = toks[idx + 1] if idx + 1 < n else None

            need_before = (
                prev is not None
                and prev.kind != "WS"
                and prev.kind not in NO_PAD_KINDS
            )
            need_after = (
                nxt is not None
                and nxt.kind != "WS"
                and nxt.kind not in NO_PAD_KINDS
            )
            if need_before:
                out.append(["WS", " ", True, False])
            out.append(["WORD", fwd_map[t.text], False, True])
            if need_after:
                out.append(["WS", " ", True, False])
        else:
            out.append([t.kind, t.text, False, False])

    text = "".join(e[1] for e in out)

    inserted_idx, subst_idx, byte_offsets = [], [], []
    off = 0
    for i, e in enumerate(out):
        if e[2]:
            inserted_idx.append(i)
            byte_offsets.append(off)
        if e[3]:
            subst_idx.append(i)
        off += len(e[1])

    posmap = {
        "inserted_ws_token_indices": inserted_idx,
        "substituted_token_indices": subst_idx,
        "inserted_ws_byte_offsets": byte_offsets,
        "transformed_token_count": len(out),
    }
    return text, posmap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--posmap")
    ap.add_argument("--mapping")
    ap.add_argument("--nocomments-out", help="write F_nocomments here (R1 reference)")
    args = ap.parse_args()

    with open(args.input, "r", encoding="utf-8") as fh:
        src = fh.read()

    doc, fwd_map = load_mapping(args.mapping)
    text, posmap = forward(src, fwd_map)

    posmap.update({
        "unit": "%s/%s/%d" % (doc["condition"], doc["language_id"], doc["seed"]),
        "source": os.path.basename(args.input),
        "note": "indices address the token stream produced by lex10 over the "
                "transformed text; inverse.py deletes exactly these WS tokens.",
    })

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)
    if args.posmap:
        with open(args.posmap, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(posmap, indent=2) + "\n")
    if args.nocomments_out:
        with open(args.nocomments_out, "w", encoding="utf-8") as fh:
            fh.write(strip_comments(src))


if __name__ == "__main__":
    main()
