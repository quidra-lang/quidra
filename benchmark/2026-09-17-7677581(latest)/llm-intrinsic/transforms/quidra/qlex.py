#!/usr/bin/env python3
"""
Lossless lexer + substitution engine shared by forward.py and inverse.py.

Token kinds: WORD, NUMBER, STRING, COMMENT, OP, WS, NEWLINE.
Lossless: "".join(t.text for t in lex(s)) == s, for every input s.

Substitution touches WHOLE WORD TOKENS ONLY. It can never reach inside a text
literal, inside a comment, or inside a longer identifier, because those are
different token kinds / different tokens.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

WORD_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
WORD_CONT = WORD_START | set("0123456789")
DIGITS = set("0123456789")


class Tok:
    __slots__ = ("kind", "text")

    def __init__(self, kind, text):
        self.kind = kind
        self.text = text

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.text)


def lex(src):
    """Lossless tokenization of this language's surface syntax."""
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "\n":
            toks.append(Tok("NEWLINE", c))
            i += 1
        elif c in " \t\r":
            j = i
            while j < n and src[j] in " \t\r":
                j += 1
            toks.append(Tok("WS", src[i:j]))
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append(Tok("COMMENT", src[i:j]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                # this language has no backslash escapes: a text literal ends at
                # the next quote character, and a literal newline may appear in it
                j += 1
            j = min(j + 1, n)
            toks.append(Tok("STRING", src[i:j]))
            i = j
        elif c in DIGITS:
            j = i
            while j < n and src[j] in DIGITS:
                j += 1
            if j < n and src[j] == "." and j + 1 < n and src[j + 1] in DIGITS:
                j += 1
                while j < n and src[j] in DIGITS:
                    j += 1
            if j < n and src[j] in "eE":
                k = j + 1
                if k < n and src[k] in "+-":
                    k += 1
                if k < n and src[k] in DIGITS:
                    j = k
                    while j < n and src[j] in DIGITS:
                        j += 1
            toks.append(Tok("NUMBER", src[i:j]))
            i = j
        elif c in WORD_START:
            j = i
            while j < n and src[j] in WORD_CONT:
                j += 1
            toks.append(Tok("WORD", src[i:j]))
            i = j
        else:
            two = src[i:i + 2]
            if two in ("==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%="):
                toks.append(Tok("OP", two))
                i += 2
            else:
                toks.append(Tok("OP", c))
                i += 1
    return toks


def render(toks):
    return "".join(t.text for t in toks)


def prev_significant(toks, idx):
    """The token before idx, skipping whitespace, newlines and comments."""
    k = idx - 1
    while k >= 0 and toks[k].kind in ("WS", "NEWLINE", "COMMENT"):
        k -= 1
    return toks[k] if k >= 0 else None


def substitute(src, table, skip_after_dot):
    """
    Replace every WORD token whose text is a key of `table`.

    skip_after_dot=True  : a WORD directly after '.' is a member name, never a
                           grammar word token, so it is left alone. Used by the
                           FORWARD direction.
    skip_after_dot=False : used by the INVERSE direction, so that a submission
                           that spells a member with a pseudo-word still maps
                           back to real source instead of failing to build.

    Returns (new_text, changed_tokens, inserted_whitespace_count).
    """
    toks = lex(src)
    changed = []
    inserted = 0
    for idx, t in enumerate(toks):
        if t.kind != "WORD" or t.text not in table:
            continue
        if skip_after_dot:
            p = prev_significant(toks, idx)
            if p is not None and p.kind == "OP" and p.text == ".":
                continue
        changed.append(t.text)
        t.text = table[t.text]
    out = render(toks)
    # emission rule (methodology 10 section 5.5): a substituted word token may
    # never end up adjacent to another word character. A WORD token can never
    # have a word character as its neighbour (maximal munch would have absorbed
    # it), so no whitespace ever has to be inserted; assert that and record 0.
    for idx, t in enumerate(toks):
        if t.kind == "WORD":
            before = toks[idx - 1].text[-1:] if idx > 0 else ""
            after = toks[idx + 1].text[:1] if idx + 1 < len(toks) else ""
            assert before not in WORD_CONT, "word-adjacency violated before %r" % t.text
            assert after not in WORD_CONT, "word-adjacency violated after %r" % t.text
    return out, changed, inserted


def load_mapping(path=None):
    path = path or os.path.join(HERE, "mapping.json")
    with open(path, "r", encoding="ascii") as fh:
        doc = json.load(fh)
    fwd = dict(doc["mapping"])
    inv = {v: k for k, v in fwd.items()}
    assert len(inv) == len(fwd), "mapping is not one-to-one"
    return doc, fwd, inv
