"""I1 forward transformer for language_id = typescript.

Text in, text out. Replaces WHOLE word tokens that are bound keywords with the
pseudo-word from mapping.json. Never touches string literals, template literals,
regular-expression literals, comments, numbers, or user identifiers.

  python3 forward.py IN.ts [OUT.ts]      (writes stdout when OUT is omitted)

When OUT is given, a position map OUT.posmap.json is written beside it recording
every whitespace character the emission rule of methodology 10 section 5.5 had to
insert, so that the inverse can delete exactly those and section 6.4 R1 stays a
plain byte-identity test. For this language the list is always empty: every bound
token is a word token, every pseudo-word is a word token of the same shape, and
two word tokens can never be adjacent in valid source without whitespace already
between them. The machinery is still present and still exercised, because a
transformer that *cannot* record an insertion would hide the defect rather than
prove it absent.

This file carries its own lexer, written independently of the one in inverse.py
(regex-driven here, a character loop there), so that a defect in one cannot
cancel itself out in the other and make the round-trip look sound.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

WORD, NUMBER, STRING, TEMPLATE, REGEX, COMMENT, WS, NEWLINE, OP = (
    "WORD", "NUMBER", "STRING", "TEMPLATE", "REGEX", "COMMENT", "WS", "NEWLINE", "OP")

# Words after which a '/' starts a regular-expression literal rather than a division.
_REGEX_OK_WORDS = frozenset("""
return typeof instanceof in of delete void new do else case yield await throw
""".split())

_MASTER = re.compile(r"""
      (?P<WS>[^\S\n]+)
    | (?P<NEWLINE>\n)
    | (?P<COMMENT>//[^\n]*|/\*[\s\S]*?\*/)
    | (?P<STRING>"(?:\\[\s\S]|[^"\\])*"|'(?:\\[\s\S]|[^'\\])*')
    | (?P<NUMBER>(?:0[xXbBoO][0-9a-fA-F_]+|\d[\d_]*(?:\.[\d_]*)?(?:[eE][+-]?\d+)?
                 |\.\d[\d_]*(?:[eE][+-]?\d+)?)n?)
    | (?P<WORD>[A-Za-z_$][A-Za-z0-9_$]*)
    | (?P<OP>[\s\S])
""", re.VERBOSE)


def _scan_template(src, i):
    """Scan a template literal starting at the backtick index i; return the end index."""
    n = len(src)
    j = i + 1
    while j < n:
        c = src[j]
        if c == "\\":
            j += 2
            continue
        if c == "`":
            return j + 1
        if c == "$" and j + 1 < n and src[j + 1] == "{":
            depth = 1
            j += 2
            while j < n and depth > 0:
                d = src[j]
                if d == "\\":
                    j += 2
                    continue
                if d in ("'", '"'):
                    q = d
                    j += 1
                    while j < n and src[j] != q:
                        j += 2 if src[j] == "\\" else 1
                    j += 1
                    continue
                if d == "`":
                    j = _scan_template(src, j)
                    continue
                if d == "{":
                    depth += 1
                elif d == "}":
                    depth -= 1
                j += 1
            continue
        j += 1
    return n


def _scan_regex(src, i):
    """Scan a regex literal starting at '/' index i; return the end index, or -1."""
    n = len(src)
    j = i + 1
    in_class = False
    while j < n:
        c = src[j]
        if c == "\n":
            return -1
        if c == "\\":
            j += 2
            continue
        if c == "[":
            in_class = True
        elif c == "]":
            in_class = False
        elif c == "/" and not in_class:
            j += 1
            while j < n and (src[j].isalpha()):
                j += 1
            return j
        j += 1
    return -1


def tokenize(src):
    """Lossless token stream: ''.join(text for _, text in tokenize(s)) == s."""
    toks = []
    i = 0
    n = len(src)
    prev = None  # last significant token, for the regex/division decision
    while i < n:
        c = src[i]
        if c == "`":
            j = _scan_template(src, i)
            toks.append((TEMPLATE, src[i:j]))
            prev = (TEMPLATE, src[i:j])
            i = j
            continue
        if c == "/" and not src.startswith("//", i) and not src.startswith("/*", i):
            divides = prev is not None and (
                prev[0] in (WORD, NUMBER, STRING, TEMPLATE, REGEX)
                and not (prev[0] == WORD and prev[1] in _REGEX_OK_WORDS)
                or (prev[0] == OP and prev[1] in (")", "]")))
            if not divides:
                j = _scan_regex(src, i)
                if j > 0:
                    toks.append((REGEX, src[i:j]))
                    prev = (REGEX, src[i:j])
                    i = j
                    continue
        m = _MASTER.match(src, i)
        kind = m.lastgroup
        text = m.group()
        toks.append((kind, text))
        if kind not in (WS, NEWLINE, COMMENT):
            prev = (kind, text)
        i = m.end()
    return toks


def load_mapping(path=None):
    with open(path or os.path.join(HERE, "mapping.json")) as f:
        doc = json.load(f)
    return {real: entry["pseudo_word"] for real, entry in doc["mapping"].items()}


def _word_char(ch):
    return ch.isalnum() or ch in "_$"


def forward_text(src, fwd=None):
    """Apply the forward (real -> anonymized) substitution to whole word tokens.

    Returns (transformed_text, inserted_offsets).
    """
    fwd = fwd if fwd is not None else load_mapping()
    reverse = set(fwd.values())
    toks = tokenize(src)
    out = []
    inserted = []
    pos = 0
    for idx, (kind, text) in enumerate(toks):
        if kind != WORD:
            out.append(text)
            pos += len(text)
            continue
        if text in reverse:
            raise ValueError(
                "FORWARD_COLLISION: source already contains the pseudo-word %r as a "
                "word token; the mapping is not injective over this source" % text)
        if text not in fwd:
            out.append(text)
            pos += len(text)
            continue
        pseudo = fwd[text]
        # Emission rule, methodology 10 section 5.5: never let a substituted word
        # token merge with a neighbouring word character. NEWLINE neighbours are
        # exempt; this language is not indentation-significant.
        if out and out[-1] and _word_char(out[-1][-1]):
            out.append(" ")
            inserted.append(pos)
            pos += 1
        out.append(pseudo)
        pos += len(pseudo)
        nxt = toks[idx + 1] if idx + 1 < len(toks) else None
        if nxt is not None and nxt[0] not in (NEWLINE,) and nxt[1] and _word_char(nxt[1][0]):
            out.append(" ")
            inserted.append(pos)
            pos += 1
    return "".join(out), inserted


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1]) as f:
        src = f.read()
    result, inserted = forward_text(src)
    if len(argv) > 2:
        with open(argv[2], "w") as f:
            f.write(result)
        with open(argv[2] + ".posmap.json", "w") as f:
            json.dump({"source": os.path.basename(argv[1]),
                       "inserted_whitespace_offsets": inserted}, f, indent=2)
            f.write("\n")
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
