#!/usr/bin/env python3
"""I1 inverse transformer for the 'rust' column: anonymized source -> real source.

  python3 inverse.py < anon.rs > real.rs
  python3 inverse.py --in anon.rs --out real.rs [--posmap anon.rs.posmap.json]

With --posmap the exact whitespace the forward transformer inserted is deleted
first, which makes  real -> forward -> inverse  byte-identical to the input.
Without it (the case for model output, which was never produced by forward)
the substitution alone is applied; the extra spaces a model may have written
are its own and are left as they are.

Every WORD token equal to a mapped pseudo-word is treated as the corresponding
keyword; every other WORD is treated as an ordinary identifier and left alone.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lex10  # noqa: E402


def load_mapping(path=None):
    p = path or os.path.join(HERE, "mapping.json")
    with open(p, "r", encoding="ascii") as f:
        return json.load(f)


def inverse(source, mapping=None, inserted_offsets=None):
    """text in, text out."""
    m = mapping or load_mapping()
    if inserted_offsets:
        source = lex10.delete_offsets(source, inserted_offsets)
    text, _ins, _ch = lex10.substitute(source, m["inverse"], insert_space=False)
    return text


def ambiguous_identifiers(source, mapping=None):
    """Words a submission used as identifiers that collide with a pseudo-word
    cannot be distinguished from role tokens by a lexer; report them so the
    caller can record INVERSE_AMBIGUOUS rather than silently mis-mapping."""
    m = mapping or load_mapping()
    pseudo = set(m["inverse"].keys())
    seen = [w for w in lex10.word_tokens(source) if w in pseudo]
    return seen


def _arg(name, default=None):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


def main():
    src_path = _arg("--in")
    out_path = _arg("--out")
    posmap_path = _arg("--posmap")
    mapping = load_mapping(_arg("--mapping"))

    source = open(src_path, "r", encoding="utf-8").read() if src_path else sys.stdin.read()

    offsets = None
    if posmap_path:
        with open(posmap_path, "r", encoding="ascii") as f:
            offsets = json.load(f)["inserted_whitespace_offsets"]

    # the inverse never inserts whitespace; it only deletes recorded insertions
    if offsets:
        source = lex10.delete_offsets(source, offsets)
    text, _ins, changed = lex10.substitute(source, mapping["inverse"], insert_space=False)

    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write("inverse: %d tokens restored\n" % len(changed))


if __name__ == "__main__":
    main()
