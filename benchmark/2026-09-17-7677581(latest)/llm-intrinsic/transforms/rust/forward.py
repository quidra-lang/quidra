#!/usr/bin/env python3
"""I1 forward transformer for the 'rust' column: real source -> anonymized source.

  python3 forward.py < real.rs > anon.rs
  python3 forward.py --in real.rs --out anon.rs [--posmap anon.rs.posmap.json]

Only whole WORD tokens that are bound keywords in mapping.json are replaced.
Text inside string literals, character literals, numbers, comments and ordinary
user identifiers is never touched, so the program's behaviour is unchanged.
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


def forward(source, mapping=None):
    """text in, text out.  Returns the anonymized text only."""
    m = mapping or load_mapping()
    text, _inserted, _changed = lex10.substitute(source, m["forward"])
    return text


def forward_detailed(source, mapping=None):
    m = mapping or load_mapping()
    return lex10.substitute(source, m["forward"])


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
    text, inserted, changed = lex10.substitute(source, mapping["forward"])

    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)

    if posmap_path:
        with open(posmap_path, "w", encoding="ascii") as f:
            json.dump({"condition": "I1", "language_id": mapping["language_id"],
                       "seed": mapping["deterministic_seed"],
                       "inserted_whitespace_offsets": inserted,
                       "substituted_tokens": [list(c) for c in changed]},
                      f, indent=2)
            f.write("\n")
    sys.stderr.write("forward: %d tokens substituted, %d whitespace insertions\n"
                     % (len(changed), len(inserted)))


if __name__ == "__main__":
    main()
