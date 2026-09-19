#!/usr/bin/env python3
"""Lossless token scanner for this language's source text.

Emits (kind, raw_text) pairs such that "".join(raw for _, raw in tokens) == source,
byte for byte.  Kinds: WORD, NUMBER, STRING, CHAR, COMMENT, OP, WS, NEWLINE.

Only WORD tokens are ever eligible for substitution.  String literals, character
literals, numeric literals and comments are never touched in either direction
(methodology 10, section 1, consequence 2).
"""

WORD_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_$")
WORD_CONT = WORD_START | set("0123456789")
DIGITS = set("0123456789")
NUM_CONT = set("0123456789abcdefABCDEF_.xXlLfFdDpP+-")


def tokenize(src):
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]

        # newline
        if c == "\n":
            toks.append(("NEWLINE", "\n"))
            i += 1
            continue

        # other whitespace runs
        if c in " \t\r\f\v":
            j = i
            while j < n and src[j] in " \t\r\f\v":
                j += 1
            toks.append(("WS", src[i:j]))
            i = j
            continue

        # comments
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append(("COMMENT", src[i:j]))
            i = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            toks.append(("COMMENT", src[i:j]))
            i = j
            continue

        # text block  """ ... """
        if src.startswith('"""', i):
            j = i + 3
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src.startswith('"""', j):
                    j += 3
                    break
                j += 1
            else:
                j = n
            toks.append(("STRING", src[i:j]))
            i = j
            continue

        # string literal
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
            toks.append(("STRING", src[i:j]))
            i = j
            continue

        # character literal
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                if src[j] == "\n":
                    break
                j += 1
            toks.append(("CHAR", src[i:j]))
            i = j
            continue

        # number literal (must be tried before OP so that '.' in 1.5 is absorbed)
        if c in DIGITS or (c == "." and i + 1 < n and src[i + 1] in DIGITS):
            j = i
            while j < n and src[j] in NUM_CONT:
                # '+'/'-' only continue a number right after an exponent marker
                if src[j] in "+-" and (j == i or src[j - 1] not in "eEpP"):
                    break
                j += 1
            toks.append(("NUMBER", src[i:j]))
            i = j
            continue

        # word token
        if c in WORD_START:
            j = i
            while j < n and src[j] in WORD_CONT:
                j += 1
            toks.append(("WORD", src[i:j]))
            i = j
            continue

        # anything else: one operator/punctuation character
        toks.append(("OP", c))
        i += 1

    return toks


def render(toks):
    return "".join(t[1] for t in toks)


def check_lossless(src):
    return render(tokenize(src)) == src


if __name__ == "__main__":
    import sys

    data = open(sys.argv[1], encoding="utf-8").read()
    ok = check_lossless(data)
    print("lossless:", ok)
    for kind, raw in tokenize(data):
        if kind in ("WORD", "STRING", "CHAR", "COMMENT"):
            print(kind, repr(raw))
    sys.exit(0 if ok else 1)
