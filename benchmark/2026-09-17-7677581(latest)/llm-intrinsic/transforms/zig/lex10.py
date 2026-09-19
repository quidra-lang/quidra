#!/usr/bin/env python3
"""
lex10 -- the single lossless lexer of methodology 10 section 6.1, instantiated with
the profile for language_id = "zig".

Token kinds emitted: WORD, NUMBER, STRING, CHAR, COMMENT, OP, WS, NEWLINE.
Losslessness invariant: "".join(t.text for t in lex(src)) == src, byte for byte.

The lexer exists so that substitution can be applied to WHOLE WORD TOKENS ONLY.
String literals (including this language's line-continued multiline string form),
character literals, numbers and comments are separate token kinds and are therefore
never reachable by the substituter -- which is what makes the methodology's
"never inside a string literal or an identifier" rule structural rather than a
matter of careful regex authoring.

Two language-specific guards, both documented in mapping.json:

  G1 -- a WORD immediately preceded by the OP "@" is a compiler-builtin name
        (`@import`, `@as`, ...) and is NEVER substituted in either direction.
        It is emitted with kind WORD but carries builtin=True.
  G2 -- `@"..."` is a quoted identifier: the "@" is an OP and the remainder is a
        STRING token, so it is unreachable by substitution by construction.
"""

PROFILE = {
    "language_id": "zig",
    "identifier_start": "letter or _",
    "identifier_continue": "letter, digit or _",
    "line_comments": ["//"],
    "block_comments": [],
    "nested_block_comments": False,
    "string_literals": [
        {"open": '"', "close": '"', "escape": "\\", "multiline": False},
        {"open": "\\\\", "close": "<end of line>", "escape": None, "multiline": True},
    ],
    "char_literals": [{"open": "'", "close": "'", "escape": "\\"}],
    "indentation_significant": False,
    "builtin_prefix": "@",
}

# longest-first (maximal munch)
OPERATORS = [
    "<<|=", ">>=", "<<=", "+%=", "-%=", "*%=", "+|=", "-|=", "*|=",
    "...", "<<|", "||=", "&&=",
    "&&", "||", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=", "<<", ">>", "+%", "-%", "*%", "+|", "-|", "*|",
    "**", "++", "->", "=>", "..", "|=",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "<", ">", "=", "!",
    "(", ")", "[", "]", "{", "}", ",", ";", ".", ":", "?", "@",
]
OPERATORS = sorted(set(OPERATORS), key=lambda s: (-len(s), s))


class Tok(object):
    __slots__ = ("kind", "text", "pos", "builtin")

    def __init__(self, kind, text, pos, builtin=False):
        self.kind = kind
        self.text = text
        self.pos = pos
        self.builtin = builtin

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.text)


class LexError(Exception):
    pass


def _is_ident_start(c):
    return c == "_" or (c.isalpha() and ord(c) < 128)


def _is_ident_cont(c):
    return c == "_" or (c.isalnum() and ord(c) < 128)


def lex(src):
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        start = i

        if c == "\n":
            toks.append(Tok("NEWLINE", "\n", start))
            i += 1
            continue

        if c in " \t\r\f\v":
            j = i
            while j < n and src[j] in " \t\r\f\v":
                j += 1
            toks.append(Tok("WS", src[i:j], start))
            i = j
            continue

        # line comment (covers //, ///, //!)
        if src.startswith("//", i):
            j = src.find("\n", i)
            if j == -1:
                j = n
            toks.append(Tok("COMMENT", src[i:j], start))
            i = j
            continue

        # multiline string literal line: \\ ... to end of line
        if src.startswith("\\\\", i):
            j = src.find("\n", i)
            if j == -1:
                j = n
            toks.append(Tok("STRING", src[i:j], start))
            i = j
            continue

        # quoted string literal
        if c == '"':
            j = i + 1
            closed = False
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    closed = True
                    break
                if src[j] == "\n":
                    raise LexError("newline in string literal at %d" % i)
                j += 1
            if not closed:
                raise LexError("unterminated string at %d" % i)
            toks.append(Tok("STRING", src[i:j], start))
            i = j
            continue

        # character literal
        if c == "'":
            j = i + 1
            closed = False
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    closed = True
                    break
                if src[j] == "\n":
                    raise LexError("newline in char literal at %d" % i)
                j += 1
            if not closed:
                raise LexError("unterminated char literal at %d" % i)
            toks.append(Tok("CHAR", src[i:j], start))
            i = j
            continue

        # number -- stops before "..", so range syntax stays two tokens
        if c.isdigit():
            j = i
            while j < n:
                if src.startswith("..", j):
                    break
                ch = src[j]
                if ch in "eEpP" and j + 1 < n and src[j + 1] in "+-":
                    j += 2
                    continue
                if ch.isalnum() or ch == "_":
                    j += 1
                    continue
                if ch == "." and j + 1 < n and src[j + 1].isdigit():
                    j += 1
                    continue
                break
            toks.append(Tok("NUMBER", src[i:j], start))
            i = j
            continue

        # word
        if _is_ident_start(c):
            j = i + 1
            while j < n and _is_ident_cont(src[j]):
                j += 1
            prev = toks[-1] if toks else None
            builtin = bool(prev and prev.kind == "OP" and prev.text == "@")
            toks.append(Tok("WORD", src[i:j], start, builtin))
            i = j
            continue

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

    rc = 0
    for path in sys.argv[1:]:
        with open(path, "r", encoding="utf-8") as fh:
            s = fh.read()
        ok = check_lossless(s)
        toks = lex(s)
        print("%s lossless=%s tokens=%d words=%d" % (
            path, ok, len(toks), sum(1 for t in toks if t.kind == "WORD")))
        if not ok:
            rc = 1
    raise SystemExit(rc)
