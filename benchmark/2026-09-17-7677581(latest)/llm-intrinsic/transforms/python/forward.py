"""I1 forward transformer for language_id = python.

Text in, text out. Replaces WHOLE word tokens that are bound keywords with the
pseudo-word from mapping.json. Never touches string literals, comments, numbers,
or user identifiers.

  python3 forward.py IN.py [OUT.py]      (writes stdout when OUT is omitted)

Invariant (methodology 10, section 6.4 R1): inverse(forward(F)) is byte-identical
to F. No whitespace is inserted: every bound token of this language is already a
word token delimited by non-word characters, and a pseudo-word is also a word
token, so substitution is length-independent and position-preserving in token
terms. Verified by validate.py.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

WORD, STRING, COMMENT, NUMBER, OTHER = "WORD", "STRING", "COMMENT", "NUMBER", "OTHER"

_ID_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
_ID_CONT = _ID_START | set("0123456789")
_DIGITS = set("0123456789")
_PREFIXES = {"r", "b", "u", "f", "rb", "br", "fr", "rf", "bf", "fb"}


def tokenize(src):
    """Lossless token stream: ''.join(text for _, text in tokenize(s)) == s."""
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "#":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append((COMMENT, src[i:j]))
            i = j
        elif c in ("'", '"'):
            i = _scan_string(src, i, "", toks)
        elif c in _ID_START:
            j = i
            while j < n and src[j] in _ID_CONT:
                j += 1
            word = src[i:j]
            if word.lower() in _PREFIXES and j < n and src[j] in ("'", '"'):
                i = _scan_string(src, j, word, toks)
            else:
                toks.append((WORD, word))
                i = j
        elif c in _DIGITS:
            j = i
            while j < n and (src[j] in _ID_CONT or src[j] == "."):
                j += 1
            toks.append((NUMBER, src[i:j]))
            i = j
        else:
            toks.append((OTHER, c))
            i += 1
    return toks


def _scan_string(src, q, prefix, toks):
    """Scan a string literal starting at the quote index q; append and return the index after it."""
    n = len(src)
    quote = src[q]
    if src[q:q + 3] in ('"""', "'''"):
        delim = src[q:q + 3]
    else:
        delim = quote
    j = q + len(delim)
    while j < n:
        if src[j] == "\\":
            j += 2
            continue
        if src.startswith(delim, j):
            j += len(delim)
            break
        j += 1
    else:
        j = n
    toks.append((STRING, prefix + src[q:j]))
    return j


def load_mapping(path=None):
    with open(path or os.path.join(HERE, "mapping.json")) as f:
        doc = json.load(f)
    return {real: entry["pseudo_word"] for real, entry in doc["mapping"].items()}


def forward_text(src, fwd=None):
    """Apply the forward (real -> anonymized) substitution to whole word tokens."""
    fwd = fwd if fwd is not None else load_mapping()
    reverse = set(fwd.values())
    out = []
    for kind, text in tokenize(src):
        if kind == WORD:
            if text in reverse:
                raise ValueError(
                    "FORWARD_COLLISION: source already contains the pseudo-word %r as a "
                    "word token; the mapping is not injective over this source" % text)
            out.append(fwd.get(text, text))
        else:
            out.append(text)
    return "".join(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1]) as f:
        src = f.read()
    result = forward_text(src)
    if len(argv) > 2:
        with open(argv[2], "w") as f:
            f.write(result)
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
