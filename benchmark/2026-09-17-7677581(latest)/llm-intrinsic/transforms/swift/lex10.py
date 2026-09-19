#!/usr/bin/env python3
"""
lex10 -- the single lossless lexer of methodology 10 section 6.1, instantiated with
the profile for language_id = "swift".

Token kinds emitted: WORD, NUMBER, STRING, CHAR, COMMENT, OP, WS, NEWLINE.
Losslessness invariant: "".join(t.text for t in lex(src)) == src, byte for byte.

The lexer exists so that substitution can be applied to WHOLE WORD TOKENS ONLY.
Text literals, numbers and comments are separate token kinds and are therefore
never reachable by the substituter -- which is what makes the methodology's
"never inside a string literal or an identifier" rule structural rather than a
matter of careful regex authoring.

Two properties of this language make the string rule load-bearing rather than
decorative:

  * a text literal may embed an expression (`\\(expr)`).  Everything between the
    `\\(` and its matching `)` is INSIDE the literal, is emitted as part of the
    STRING token, and is therefore untouchable in both directions -- exactly what
    methodology 10 section 1 consequence 2 and section 3.2 V18 require.  The
    scanner tracks parenthesis depth and nested literals so that the STRING token
    ends at the right quote even when an embedded expression itself contains a
    literal or a parenthesis.
  * this language has extended (`#"..."#`) and multi-line (`\"\"\"`) literal forms.
    They are recognized here so that a submission written with them still lexes
    losslessly instead of derailing the substituter.
"""

PROFILE = {
    "language_id": "swift",
    "identifier_start": "letter or _",
    "identifier_continue": "letter, digit or _",
    "line_comments": ["//"],
    "block_comments": [("/*", "*/")],
    "nested_block_comments": True,
    "string_literals": [
        {"open": '"""', "close": '"""', "escape": "\\", "multiline": True},
        {"open": '"', "close": '"', "escape": "\\", "multiline": False},
        {"open": '#"', "close": '"#', "escape": "\\#", "multiline": False,
         "note": "extended delimiters; any number of '#'"},
    ],
    "char_literals": [],          # this language has no separate character literal
    "interpolation": "\\(...) inside a text literal; never transformed (V18)",
    "indentation_significant": False,
}

# longest-first (maximal munch)
OPERATORS = [
    "...", "..<", "&&=", "||=", "<<=", ">>=", "===", "!==", "&+", "&-", "&*",
    "->", "??", "&&", "||", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "<<", ">>", "?.",
    "+", "-", "*", "/", "%", "&", "|", "^", "<", ">", "=", "!", "~", "?",
    "(", ")", "[", "]", "{", "}", ",", ";", ".", ":", "@", "#", "_", "$", "\\",
]


class Tok(object):
    __slots__ = ("kind", "text", "pos")

    def __init__(self, kind, text, pos):
        self.kind = kind
        self.text = text
        self.pos = pos

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.text)


class LexError(Exception):
    pass


def _is_ident_start(c):
    return c == "_" or c.isalpha()


def _is_ident_cont(c):
    return c == "_" or c.isalnum()


def _scan_plain_string(src, i, triple):
    """Scan a `"` or `\"\"\"` literal starting at i.  Returns the index just past it.

    Handles backslash escapes and `\\(` ... `)` embedded expressions, including an
    embedded expression that itself contains a text literal or parentheses.
    """
    n = len(src)
    delim = '"""' if triple else '"'
    j = i + len(delim)
    while j < n:
        c = src[j]
        if c == "\\":
            if j + 1 < n and src[j + 1] == "(":
                j = _skip_interpolation(src, j + 2)
                continue
            j += 2
            continue
        if src.startswith(delim, j):
            return j + len(delim)
        if c == "\n" and not triple:
            raise LexError("line end inside a single-line text literal at %d" % i)
        j += 1
    raise LexError("unterminated text literal at %d" % i)


def _skip_interpolation(src, j):
    """j points just past `\\(`.  Return the index just past the matching `)`."""
    n = len(src)
    depth = 1
    while j < n:
        c = src[j]
        if c == '"':
            j = _scan_plain_string(src, j, src.startswith('"""', j))
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    raise LexError("unterminated embedded expression at %d" % j)


def _scan_extended_string(src, i):
    """Scan `#...#"` ... `"#...#` starting at i (which points at the first `#`)."""
    n = len(src)
    k = i
    while k < n and src[k] == "#":
        k += 1
    pounds = k - i
    if k >= n or src[k] != '"':
        return None
    triple = src.startswith('"""', k)
    open_q = '"""' if triple else '"'
    close = open_q + ("#" * pounds)
    j = k + len(open_q)
    esc = "\\" + ("#" * pounds)
    while j < n:
        if src.startswith(esc, j):
            if src.startswith(esc + "(", j):
                j = _skip_interpolation(src, j + len(esc) + 1)
                continue
            j += len(esc) + 1
            continue
        if src.startswith(close, j):
            return j + len(close)
        j += 1
    raise LexError("unterminated extended text literal at %d" % i)


def lex(src):
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        start = i

        # newline
        if c == "\n":
            toks.append(Tok("NEWLINE", "\n", start))
            i += 1
            continue

        # horizontal whitespace run
        if c in " \t\r\f\v":
            j = i
            while j < n and src[j] in " \t\r\f\v":
                j += 1
            toks.append(Tok("WS", src[i:j], start))
            i = j
            continue

        # comments
        if src.startswith("//", i):
            j = src.find("\n", i)
            if j == -1:
                j = n
            toks.append(Tok("COMMENT", src[i:j], start))
            i = j
            continue
        if src.startswith("/*", i):
            depth = 1
            j = i + 2
            while j < n and depth:
                if src.startswith("/*", j):
                    depth += 1
                    j += 2
                elif src.startswith("*/", j):
                    depth -= 1
                    j += 2
                else:
                    j += 1
            if depth:
                raise LexError("unterminated block comment at %d" % i)
            toks.append(Tok("COMMENT", src[i:j], start))
            i = j
            continue

        # extended text literal  #"..."#   (must be tried before the `#` operator)
        if c == "#":
            j = _scan_extended_string(src, i)
            if j is not None:
                toks.append(Tok("STRING", src[i:j], start))
                i = j
                continue

        # multi-line and single-line text literals
        if src.startswith('"""', i):
            j = _scan_plain_string(src, i, True)
            toks.append(Tok("STRING", src[i:j], start))
            i = j
            continue
        if c == '"':
            j = _scan_plain_string(src, i, False)
            toks.append(Tok("STRING", src[i:j], start))
            i = j
            continue

        # number
        if c.isdigit():
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"
                             or (src[j] == "." and j + 1 < n and src[j + 1].isdigit())):
                if src[j] in "eEpP" and j + 1 < n and src[j + 1] in "+-":
                    j += 2
                    continue
                j += 1
            toks.append(Tok("NUMBER", src[i:j], start))
            i = j
            continue

        # word
        if _is_ident_start(c) and c != "_":
            j = i + 1
            while j < n and _is_ident_cont(src[j]):
                j += 1
            toks.append(Tok("WORD", src[i:j], start))
            i = j
            continue
        if c == "_":
            # `_` alone is the wildcard pattern (an OP); `_foo` is a word.
            j = i + 1
            while j < n and _is_ident_cont(src[j]):
                j += 1
            if j == i + 1:
                toks.append(Tok("OP", "_", start))
            else:
                toks.append(Tok("WORD", src[i:j], start))
            i = j
            continue

        # backtick-escaped identifier: `for` is a WORD spelled with backticks.
        # It is deliberately NOT a substitution site: the backticks say "this is a
        # name, not a grammar word", which is precisely the case the transformer
        # must leave alone.
        if c == "`":
            j = src.find("`", i + 1)
            if j == -1:
                raise LexError("unterminated escaped identifier at %d" % i)
            toks.append(Tok("CHAR", src[i:j + 1], start))
            i = j + 1
            continue

        # operator / punctuation (maximal munch)
        for op in OPERATORS:
            if src.startswith(op, i):
                toks.append(Tok("OP", op, start))
                i += len(op)
                break
        else:
            raise LexError("unexpected character %r at %d" % (c, i))

    return toks


def unlex(toks):
    return "".join(t.text for t in toks)


def check_lossless(src):
    return unlex(lex(src)) == src


if __name__ == "__main__":
    import sys

    for path in sys.argv[1:]:
        with open(path, "r", encoding="utf-8") as fh:
            s = fh.read()
        ok = check_lossless(s)
        print("%s lossless=%s tokens=%d" % (path, ok, len(lex(s))))
        if not ok:
            sys.exit(1)
