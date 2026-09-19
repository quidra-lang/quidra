#!/usr/bin/env python3
"""
Lossless C++ lexer for the I1 transformers (methodology 10 §6.1, `lex10` profile
for language_id = "cpp").

Emits typed tokens: WORD, NUMBER, STRING, CHAR, COMMENT, OP, WS, NEWLINE.
Losslessness invariant: "".join(t.text for t in lex(s)) == s, for any input.

Only WORD tokens are ever rewritten by forward/inverse. STRING, CHAR, NUMBER and
COMMENT are never touched in either direction (§1 consequences 2 and 3).
"""

ID_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_$")
ID_CONT = ID_START | set("0123456789")
DIGITS = set("0123456789")
WS_CHARS = set(" \t\r\f\v")


class Tok(object):
    __slots__ = ("kind", "text", "pos")

    def __init__(self, kind, text, pos):
        self.kind = kind
        self.text = text
        self.pos = pos

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.text)


def _raw_string(s, i):
    """R"delim( ... )delim"  -> (end_index) or None if not a raw string here."""
    # s[i] == 'R' and s[i+1] == '"'
    j = i + 2
    delim = ""
    while j < len(s) and s[j] != "(":
        if s[j] in '"\\ \t\r\n' or len(delim) > 16:
            return None
        delim += s[j]
        j += 1
    if j >= len(s):
        return None
    close = ")" + delim + '"'
    k = s.find(close, j + 1)
    if k < 0:
        return len(s)
    return k + len(close)


def lex(src):
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]

        if c == "\n":
            toks.append(Tok("NEWLINE", "\n", i))
            i += 1
            continue

        if c in WS_CHARS:
            j = i
            while j < n and src[j] in WS_CHARS:
                j += 1
            toks.append(Tok("WS", src[i:j], i))
            i = j
            continue

        # comments
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            if j < 0:
                j = n
            toks.append(Tok("COMMENT", src[i:j], i))
            i = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            toks.append(Tok("COMMENT", src[i:j], i))
            i = j
            continue

        # raw string literal (possibly prefixed: R, uR, u8R, LR, UR)
        if c in "RuUL":
            k = i
            while k < n and src[k] in "uU8L":
                k += 1
            if k < n and src[k] == "R" and k + 1 < n and src[k + 1] == '"':
                end = _raw_string(src, k)
                if end is not None:
                    toks.append(Tok("STRING", src[i:end], i))
                    i = end
                    continue

        # ordinary string literal (with optional encoding prefix already consumed
        # as part of a WORD if it was one; handled here when quote is immediate)
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                if src[j] == "\n":
                    break
                j += 1
            toks.append(Tok("STRING", src[i:j], i))
            i = j
            continue

        # character literal
        if c == "'":
            j = i + 1
            ok = False
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    ok = True
                    break
                if src[j] == "\n":
                    break
                j += 1
            if ok:
                toks.append(Tok("CHAR", src[i:j], i))
                i = j
                continue
            toks.append(Tok("OP", "'", i))
            i += 1
            continue

        # number (pp-number, incl. digit separators and exponent signs)
        if c in DIGITS or (c == "." and i + 1 < n and src[i + 1] in DIGITS):
            j = i
            while j < n:
                ch = src[j]
                if ch in ID_CONT or ch == ".":
                    if ch in "eEpP" and j + 1 < n and src[j + 1] in "+-":
                        j += 2
                        continue
                    j += 1
                    continue
                if ch == "'" and j + 1 < n and src[j + 1] in ID_CONT:
                    j += 2
                    continue
                break
            toks.append(Tok("NUMBER", src[i:j], i))
            i = j
            continue

        # identifier / keyword
        if c in ID_START:
            j = i
            while j < n and src[j] in ID_CONT:
                j += 1
            toks.append(Tok("WORD", src[i:j], i))
            i = j
            continue

        # anything else: one operator/punctuator character
        toks.append(Tok("OP", c, i))
        i += 1

    return toks


def render(toks):
    return "".join(t.text for t in toks)


if __name__ == "__main__":
    import sys

    data = open(sys.argv[1], "r", encoding="utf-8").read()
    ts = lex(data)
    assert render(ts) == data, "lexer is not lossless"
    print("lossless OK: %d tokens, %d WORD" % (len(ts), sum(1 for t in ts if t.kind == "WORD")))
