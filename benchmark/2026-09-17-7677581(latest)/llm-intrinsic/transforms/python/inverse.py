"""I1 inverse transformer for language_id = python.

Text in, text out. Replaces WHOLE word tokens that are pseudo-words with the real
keyword they stand for. Never touches string literals, comments, numbers, or user
identifiers. This is the only direction ever applied to a model submission, and
only its output is ever handed to the real toolchain (methodology 10, 6.5).

  python3 inverse.py IN.py [OUT.py]      (writes stdout when OUT is omitted)

Self-contained on purpose: it shares no code with forward.py, so a defect in one
lexer cannot cancel itself out in the other and make the round-trip look sound.
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


def load_inverse_mapping(path=None):
    with open(path or os.path.join(HERE, "mapping.json")) as f:
        doc = json.load(f)
    inv = {}
    for real, entry in doc["mapping"].items():
        pseudo = entry["pseudo_word"]
        if pseudo in inv:
            raise ValueError("MAPPING_NOT_INJECTIVE: %r maps from two roles" % pseudo)
        inv[pseudo] = real
    return inv


def inverse_text(src, inv=None):
    """Apply the inverse (anonymized -> real) substitution to whole word tokens."""
    inv = inv if inv is not None else load_inverse_mapping()
    return "".join(
        inv.get(text, text) if kind == WORD else text for kind, text in tokenize(src))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1]) as f:
        src = f.read()
    result = inverse_text(src)
    if len(argv) > 2:
        with open(argv[2], "w") as f:
            f.write(result)
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
