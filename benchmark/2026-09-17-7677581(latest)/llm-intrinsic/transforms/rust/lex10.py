#!/usr/bin/env python3
"""lex10 - the single lossless lexer used by both directions of the I1
transformation for the 'rust' column.

Emits typed tokens: WORD, NUMBER, STRING, CHAR, LIFETIME, COMMENT, OP, WS,
NEWLINE.  Losslessness invariant: "".join(t.text for t in lex(s)) == s.

Only WORD tokens are ever eligible for substitution.  STRING, CHAR, NUMBER and
COMMENT text is never inspected and never modified (methodology section 1,
consequence 2), which is what keeps program output unchanged.
"""

ID_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
ID_CONT = ID_START | set("0123456789")
DIGITS = set("0123456789")
WS_CHARS = set(" \t\r\x0b\x0c")


class Tok(object):
    __slots__ = ("kind", "text")

    def __init__(self, kind, text):
        self.kind = kind
        self.text = text

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.text)


def _raw_string_at(s, i):
    """If a raw string literal starts at i, return its end index, else None.
    Handles r"..", r#".."#, r##".."##, and the b-prefixed byte forms."""
    j = i
    if j < len(s) and s[j] == "b":
        j += 1
    if j >= len(s) or s[j] != "r":
        return None
    j += 1
    hashes = 0
    while j < len(s) and s[j] == "#":
        hashes += 1
        j += 1
    if j >= len(s) or s[j] != '"':
        return None
    j += 1
    closer = '"' + "#" * hashes
    k = s.find(closer, j)
    if k < 0:
        return len(s)
    return k + len(closer)


def lex(s):
    toks = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        # newline
        if c == "\n":
            toks.append(Tok("NEWLINE", "\n"))
            i += 1
            continue
        # horizontal whitespace run
        if c in WS_CHARS:
            j = i
            while j < n and s[j] in WS_CHARS:
                j += 1
            toks.append(Tok("WS", s[i:j]))
            i = j
            continue
        # line comment
        if s.startswith("//", i):
            j = s.find("\n", i)
            if j < 0:
                j = n
            toks.append(Tok("COMMENT", s[i:j]))
            i = j
            continue
        # block comment (nested)
        if s.startswith("/*", i):
            depth = 0
            j = i
            while j < n:
                if s.startswith("/*", j):
                    depth += 1
                    j += 2
                elif s.startswith("*/", j):
                    depth -= 1
                    j += 2
                    if depth == 0:
                        break
                else:
                    j += 1
            toks.append(Tok("COMMENT", s[i:j]))
            i = j
            continue
        # raw / byte-raw string
        if c in ("r", "b"):
            end = _raw_string_at(s, i)
            if end is not None:
                toks.append(Tok("STRING", s[i:end]))
                i = end
                continue
        # ordinary string (optionally byte-prefixed)
        if c == '"' or (c == "b" and i + 1 < n and s[i + 1] == '"'):
            j = i + (1 if c == '"' else 2)
            while j < n:
                if s[j] == "\\":
                    j += 2
                    continue
                if s[j] == '"':
                    j += 1
                    break
                j += 1
            toks.append(Tok("STRING", s[i:j]))
            i = j
            continue
        # char literal vs lifetime
        if c == "'" or (c == "b" and i + 1 < n and s[i + 1] == "'"):
            start = i
            j = i + (1 if c == "'" else 2)
            if j < n and s[j] == "\\":
                k = j + 1
                while k < n and s[k] != "'":
                    k += 1
                toks.append(Tok("CHAR", s[start:k + 1]))
                i = k + 1
                continue
            # a lifetime is 'ident NOT followed by a closing quote
            if j < n and s[j] in ID_START:
                k = j
                while k < n and s[k] in ID_CONT:
                    k += 1
                if k < n and s[k] == "'":
                    toks.append(Tok("CHAR", s[start:k + 1]))
                    i = k + 1
                else:
                    toks.append(Tok("LIFETIME", s[start:k]))
                    i = k
                continue
            if j + 1 < n and s[j + 1] == "'":
                toks.append(Tok("CHAR", s[start:j + 2]))
                i = j + 2
                continue
            toks.append(Tok("OP", c))
            i += 1
            continue
        # number
        if c in DIGITS:
            j = i
            while j < n and (s[j] in ID_CONT or s[j] == "."):
                # do not swallow a range operator ".."
                if s[j] == "." and j + 1 < n and s[j + 1] == ".":
                    break
                if s[j] == "." and not (j + 1 < n and s[j + 1] in DIGITS):
                    break
                j += 1
            toks.append(Tok("NUMBER", s[i:j]))
            i = j
            continue
        # word
        if c in ID_START:
            j = i
            while j < n and s[j] in ID_CONT:
                j += 1
            toks.append(Tok("WORD", s[i:j]))
            i = j
            continue
        # anything else is punctuation, one character at a time
        toks.append(Tok("OP", c))
        i += 1
    return toks


def render(toks):
    return "".join(t.text for t in toks)


def substitute(source, table, drop_comments=False, insert_space=True):
    """Replace every whole WORD token found in `table`.  Returns
    (text, inserted_offsets, changed_tokens).

    Emission rule (methodology section 5.5): a substituted word token is emitted
    surrounded by the whitespace that was already there, and by a single
    inserted space where there was none.  No space is inserted when the
    neighbouring token is NEWLINE (nor inside a leading-indent run; this
    language's lexer profile is not indentation-significant).  Every inserted
    character's offset in the produced text is recorded so that the inverse can
    delete exactly those insertions and restore byte identity.
    """
    toks = lex(source)
    if drop_comments:
        kept = []
        for idx, t in enumerate(toks):
            if t.kind != "COMMENT":
                kept.append(t)
                continue
            # never let deletion merge the two neighbours into one token
            left = kept[-1].text[-1:] if kept else ""
            right = toks[idx + 1].text[:1] if idx + 1 < len(toks) else ""
            if left and right and not left.isspace() and not right.isspace():
                kept.append(Tok("WS", " "))
        toks = kept
    out = []
    inserted = []
    changed = []
    pos = 0
    for idx, t in enumerate(toks):
        if t.kind == "WORD" and t.text in table:
            prev_text = out[-1] if out else ""
            need_before = insert_space and bool(prev_text) and not prev_text[-1].isspace()
            if need_before:
                inserted.append(pos)
                out.append(" ")
                pos += 1
            repl = table[t.text]
            changed.append((t.text, repl))
            out.append(repl)
            pos += len(repl)
            nxt_text = toks[idx + 1].text if idx + 1 < len(toks) else ""
            need_after = insert_space and bool(nxt_text) and not nxt_text[0].isspace()
            if need_after:
                inserted.append(pos)
                out.append(" ")
                pos += 1
        else:
            out.append(t.text)
            pos += len(t.text)
    return "".join(out), inserted, changed


def delete_offsets(text, offsets):
    keep = set(offsets)
    return "".join(ch for k, ch in enumerate(text) if k not in keep)


def word_tokens(source):
    return [t.text for t in lex(source) if t.kind == "WORD"]
