#!/usr/bin/env python3
"""
lex10 -- the single lossless lexer of methodology 10 section 6.1, instantiated with
the profile for language_id = "go".

Token kinds emitted: WORD, NUMBER, STRING, CHAR, COMMENT, OP, WS, NEWLINE.
Losslessness invariant: "".join(t.text for t in lex(src)) == src, byte for byte.

The lexer exists so that substitution can be applied to WHOLE WORD TOKENS ONLY.
String literals, rune literals, numbers and comments are separate token kinds and
are therefore never reachable by the substituter -- which is what makes the
methodology's "never inside a string literal or an identifier" rule structural
rather than a matter of careful regex authoring.
"""

PROFILE = {
    "language_id": "go",
    "identifier_start": "letter or _",
    "identifier_continue": "letter, digit or _",
    "line_comments": ["//"],
    "block_comments": [("/*", "*/")],
    "nested_block_comments": False,
    "string_literals": [
        {"open": '"', "close": '"', "escape": "\\", "multiline": False},
        {"open": "`", "close": "`", "escape": None, "multiline": True},
    ],
    "char_literals": [{"open": "'", "close": "'", "escape": "\\"}],
    "indentation_significant": False,
}

# longest-first (maximal munch)
OPERATORS = [
    "<<=", ">>=", "&^=", "...",
    "&&", "||", "<-", "++", "--", "==", "!=", "<=", ">=", ":=",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "&^",
    "+", "-", "*", "/", "%", "&", "|", "^", "<", ">", "=", "!",
    "(", ")", "[", "]", "{", "}", ",", ";", ".", ":", "~",
]


class Tok(object):
    __slots__ = ("kind", "text", "pos")

    def __init__(self, kind, text, pos):
        self.kind = kind
        self.text = text
        self.pos = pos

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.text)


def _is_ident_start(c):
    return c == "_" or c.isalpha()


def _is_ident_cont(c):
    return c == "_" or c.isalnum()


class LexError(Exception):
    pass


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
            j = src.find("*/", i + 2)
            if j == -1:
                raise LexError("unterminated block comment at %d" % i)
            j += 2
            toks.append(Tok("COMMENT", src[i:j], start))
            i = j
            continue

        # interpreted string literal
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
                    raise LexError("newline in interpreted string at %d" % i)
                j += 1
            else:
                raise LexError("unterminated string at %d" % i)
            toks.append(Tok("STRING", src[i:j], start))
            i = j
            continue

        # raw string literal
        if c == "`":
            j = src.find("`", i + 1)
            if j == -1:
                raise LexError("unterminated raw string at %d" % i)
            j += 1
            toks.append(Tok("STRING", src[i:j], start))
            i = j
            continue

        # rune literal
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                j += 1
            else:
                raise LexError("unterminated rune literal at %d" % i)
            toks.append(Tok("CHAR", src[i:j], start))
            i = j
            continue

        # number
        if c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            j = i
            while j < n and (src[j].isalnum() or src[j] == "." or src[j] == "_"):
                # exponent sign
                if src[j] in "eEpP" and j + 1 < n and src[j + 1] in "+-":
                    j += 2
                    continue
                j += 1
            toks.append(Tok("NUMBER", src[i:j], start))
            i = j
            continue

        # word
        if _is_ident_start(c):
            j = i + 1
            while j < n and _is_ident_cont(src[j]):
                j += 1
            toks.append(Tok("WORD", src[i:j], start))
            i = j
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
