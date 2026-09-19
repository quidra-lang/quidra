#!/usr/bin/env python3
"""Lossless token scanner for this language's source text.

Emits (kind, raw_text) pairs such that "".join(raw for _, raw in tokens) == source,
byte for byte.  Kinds: WORD, BQWORD, NUMBER, STRING, CHAR, COMMENT, OP, WS, NEWLINE.

Only WORD tokens are ever eligible for substitution.  String literals, character
literals, numeric literals, back-quoted identifiers and comments are never touched
in either direction (methodology 10, section 1, consequence 2; section 6.2 last line).

Three things this scanner must get right that a Java-shaped scanner does not:

  1. The range operator.  `0..49` is NUMBER `0`, OP `..`, NUMBER `49` -- not one
     number token.  A `.` is absorbed into a number only when a DIGIT follows it.
  2. Nested block comments.  `/* /* */ */` is one comment in this language.
  3. Back-quoted identifiers.  `` `if` `` is an identifier, not the keyword, so it
     is emitted as BQWORD and is never substituted in either direction.

Multi-character operators are matched longest-first (maximal munch) so that `..`,
`===`, `?:`, `!in` and `!is` are single tokens.
"""

WORD_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
WORD_CONT = WORD_START | set("0123456789")
DIGITS = set("0123456789")
HEXDIGITS = set("0123456789abcdefABCDEF")

# longest-first; every multi-character operator this language spells
OPS = [
    "...", "===", "!==", "?.:",
    "..<", "<<=", ">>=",
    "..", "::", "->", "=>", "?.", "?:", "++", "--", "+=", "-=", "*=", "/=", "%=",
    "==", "!=", "<=", ">=", "&&", "||", "!!",
]
OPS = sorted(set(OPS), key=len, reverse=True)


def _scan_number(src, i, n):
    """Return the end index of the number literal starting at src[i]."""
    j = i
    if src[j] == "0" and j + 1 < n and src[j + 1] in "xX":
        j += 2
        while j < n and (src[j] in HEXDIGITS or src[j] == "_"):
            j += 1
    elif src[j] == "0" and j + 1 < n and src[j + 1] in "bB":
        j += 2
        while j < n and (src[j] in "01_"):
            j += 1
    else:
        while j < n and (src[j] in DIGITS or src[j] == "_"):
            j += 1
        # a '.' continues the number ONLY when a digit follows it; this is what
        # keeps `0..49` from being swallowed as a single token
        if j < n and src[j] == "." and j + 1 < n and src[j + 1] in DIGITS:
            j += 1
            while j < n and (src[j] in DIGITS or src[j] == "_"):
                j += 1
        if j < n and src[j] in "eE":
            k = j + 1
            if k < n and src[k] in "+-":
                k += 1
            if k < n and src[k] in DIGITS:
                j = k
                while j < n and src[j] in DIGITS:
                    j += 1
    # type suffix
    if j < n and src[j] in "lLfFdDuU":
        j += 1
        if j < n and src[j] in "lL":      # e.g. 1uL
            j += 1
    return j


def tokenize(src):
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]

        if c == "\n":
            toks.append(("NEWLINE", "\n"))
            i += 1
            continue

        if c in " \t\r\f\v":
            j = i
            while j < n and src[j] in " \t\r\f\v":
                j += 1
            toks.append(("WS", src[i:j]))
            i = j
            continue

        # line comment
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append(("COMMENT", src[i:j]))
            i = j
            continue

        # block comment, NESTED
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            depth = 1
            j = i + 2
            while j < n and depth > 0:
                if src.startswith("/*", j):
                    depth += 1
                    j += 2
                elif src.startswith("*/", j):
                    depth -= 1
                    j += 2
                else:
                    j += 1
            toks.append(("COMMENT", src[i:j]))
            i = j
            continue

        # raw string  """ ... """
        if src.startswith('"""', i):
            j = i + 3
            while j < n:
                if src.startswith('"""', j):
                    j += 3
                    # a raw string ends at the LAST of a run of quotes
                    while j < n and src[j] == '"':
                        j += 1
                    break
                j += 1
            else:
                j = n
            toks.append(("STRING", src[i:j]))
            i = j
            continue

        # escaped string
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

        # back-quoted identifier: never a keyword, never substituted
        if c == "`":
            j = i + 1
            while j < n and src[j] != "`" and src[j] != "\n":
                j += 1
            if j < n and src[j] == "`":
                j += 1
            toks.append(("BQWORD", src[i:j]))
            i = j
            continue

        # number literal
        if c in DIGITS:
            j = _scan_number(src, i, n)
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

        # multi-character operator, longest first
        matched = None
        for op in OPS:
            if src.startswith(op, i):
                matched = op
                break
        if matched:
            toks.append(("OP", matched))
            i += len(matched)
            continue

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
        if kind in ("WORD", "BQWORD", "STRING", "CHAR", "COMMENT", "NUMBER"):
            print(kind, repr(raw))
    sys.exit(0 if ok else 1)
