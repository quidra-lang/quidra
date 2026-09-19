"""I1 inverse transformer for language_id = typescript.

Text in, text out. Replaces WHOLE word tokens that are pseudo-words with the real
keyword they stand for. Never touches string literals, template literals,
regular-expression literals, comments, numbers, or user identifiers. This is the
only direction ever applied to a model submission, and only its output is ever
handed to the real toolchain (methodology 10, 6.5).

  python3 inverse.py IN.ts [OUT.ts] [--posmap P.json]

  --posmap deletes exactly the whitespace characters that forward.py recorded
  inserting (methodology 10, 5.5), which is what makes 6.4 R1 a plain
  byte-identity test. A model submission has no position map and needs none.

Self-contained on purpose: the lexer below is a hand-written character loop,
written independently of the regex-driven lexer in forward.py, so a defect in one
cannot cancel itself out in the other and make the round-trip look sound.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

WORD, NUMBER, STRING, TEMPLATE, REGEX, COMMENT, WS, OP = (
    "WORD", "NUMBER", "STRING", "TEMPLATE", "REGEX", "COMMENT", "WS", "OP")

_ID_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_$")
_ID_CONT = _ID_START | set("0123456789")
_DIGITS = set("0123456789")
_NUM_CONT = _ID_CONT | set(".")

_REGEX_OK_WORDS = frozenset("""
return typeof instanceof in of delete void new do else case yield await throw
""".split())


def tokenize(src, keyword_words=frozenset()):
    """Lossless token stream: ''.join(text for _, text in tokenize(s)) == s.

    `keyword_words` is the set of word tokens after which a '/' begins a regular
    expression rather than a division; the caller adds the pseudo-words, because
    in anonymized text they occupy the grammar positions the real words do.
    """
    toks = []
    i = 0
    n = len(src)
    prev_kind = None
    prev_text = ""
    while i < n:
        c = src[i]

        if c in " \t\r\n\f\v":
            j = i
            while j < n and src[j] in " \t\r\n\f\v":
                j += 1
            toks.append((WS, src[i:j]))
            i = j
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append((COMMENT, src[i:j]))
            i = j
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            toks.append((COMMENT, src[i:j]))
            i = j
            continue

        if c == "/":
            after_value = (
                prev_kind in (NUMBER, STRING, TEMPLATE, REGEX)
                or (prev_kind == WORD and prev_text not in _REGEX_OK_WORDS
                    and prev_text not in keyword_words)
                or (prev_kind == OP and prev_text in (")", "]")))
            if not after_value:
                j = _end_of_regex(src, i)
                if j > 0:
                    toks.append((REGEX, src[i:j]))
                    prev_kind, prev_text = REGEX, src[i:j]
                    i = j
                    continue

        if c == '"' or c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                j += 1
            else:
                j = n
            toks.append((STRING, src[i:j]))
            prev_kind, prev_text = STRING, src[i:j]
            i = j
            continue

        if c == "`":
            j = _end_of_template(src, i)
            toks.append((TEMPLATE, src[i:j]))
            prev_kind, prev_text = TEMPLATE, src[i:j]
            i = j
            continue

        if c in _ID_START:
            j = i
            while j < n and src[j] in _ID_CONT:
                j += 1
            toks.append((WORD, src[i:j]))
            prev_kind, prev_text = WORD, src[i:j]
            i = j
            continue

        if c in _DIGITS or (c == "." and i + 1 < n and src[i + 1] in _DIGITS):
            j = i + 1
            while j < n and (src[j] in _NUM_CONT
                             or (src[j] in "+-" and src[j - 1] in "eE")):
                j += 1
            toks.append((NUMBER, src[i:j]))
            prev_kind, prev_text = NUMBER, src[i:j]
            i = j
            continue

        toks.append((OP, c))
        prev_kind, prev_text = OP, c
        i += 1
    return toks


def _end_of_template(src, i):
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
                    j = _end_of_template(src, j)
                    continue
                if d == "{":
                    depth += 1
                elif d == "}":
                    depth -= 1
                j += 1
            continue
        j += 1
    return n


def _end_of_regex(src, i):
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
            while j < n and src[j].isalpha():
                j += 1
            return j
        j += 1
    return -1


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


def inverse_text(src, inv=None, inserted=()):
    """Apply the inverse (anonymized -> real) substitution to whole word tokens.

    `inserted` holds offsets INTO `src` (the transformed text) of whitespace the
    forward emission rule inserted; those exact characters are deleted, which is
    what makes methodology 10, 6.4 R1 a plain byte-identity test. Deletion is done
    on the token stream, so a dropped space can never merge two tokens silently.
    """
    inv = inv if inv is not None else load_inverse_mapping()
    drop = set(inserted)
    out = []
    pos = 0
    for kind, text in tokenize(src, keyword_words=frozenset(inv)):
        if kind == WORD:
            out.append(inv.get(text, text))
        elif drop:
            out.append("".join(ch for k, ch in enumerate(text) if pos + k not in drop))
        else:
            out.append(text)
        pos += len(text)
    return "".join(out)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    posmap = None
    for k, a in enumerate(argv):
        if a == "--posmap" and k + 1 < len(argv):
            posmap = argv[k + 1]
            args = [x for x in args if x != posmap]
    if not args:
        print(__doc__)
        return 2
    with open(args[0]) as f:
        src = f.read()
    inserted = ()
    if posmap:
        with open(posmap) as f:
            inserted = tuple(json.load(f)["inserted_whitespace_offsets"])
    result = inverse_text(src, inserted=inserted)
    if len(args) > 1:
        with open(args[1], "w") as f:
            f.write(result)
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
