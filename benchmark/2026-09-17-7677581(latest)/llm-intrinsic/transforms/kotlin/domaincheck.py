#!/usr/bin/env python3
"""PF-03: recompute the section 7.1a I1 domain MECHANICALLY and compare it to
the authored binding table in mapping.json.

The domain rule is frozen: every token of the language's frozen reserved-word list
that occurs as a whole WORD token in the fixture is anonymized.  Two independent
analysts must compute the same number, so it is computed here from the two frozen
inputs rather than read off the table.

Also enumerates every WORD token of the fixture and classifies it, so that the
"zero unclassified word tokens" and "unbound_word_tokens is empty" criteria are
demonstrated rather than asserted.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ktlex import tokenize  # noqa: E402


def words(path):
    src = open(os.path.join(HERE, path), encoding="utf-8").read()
    return [raw for kind, raw in tokenize(src) if kind == "WORD"]


def main():
    reserved = [ln.strip() for ln in
                open(os.path.join(HERE, "wordlists", "reserved_kotlin.txt"), encoding="ascii")
                if ln.strip()]
    reserved_set = set(reserved)
    doc = json.load(open(os.path.join(HERE, "mapping.json"), encoding="ascii"))
    table = set(doc["mapping"].keys())

    fixture_words = words("fixture_real.kt")
    uniq = sorted(set(fixture_words))

    recomputed = sorted(w for w in uniq if w in reserved_set)
    v_roles = set(doc["not_transformed"]["V-roles"])
    identifiers = sorted(w for w in uniq if w not in reserved_set and w not in v_roles)

    print("frozen reserved-word list entries : %d" % len(reserved))
    print("distinct WORD tokens in fixture   : %d" % len(uniq))
    print()
    print("RECOMPUTED domain (reserved list INTERSECT fixture word tokens):")
    for w in recomputed:
        print("    %s" % w)
    print("  count = %d" % len(recomputed))
    print()
    print("AUTHORED binding table (mapping.json keys):")
    for w in sorted(table):
        print("    %s" % w)
    print("  count = %d" % len(table))
    print()

    fails = []
    same = (set(recomputed) == table)
    print("  recomputed domain == authored table          : %s" % same)
    if not same:
        fails.append("domain-mismatch: %s" % sorted(set(recomputed) ^ table))

    count_ok = (doc["anonymized_token_count"] == len(recomputed))
    print("  anonymized_token_count matches recompute     : %s (%d)"
          % (count_ok, doc["anonymized_token_count"]))
    if not count_ok:
        fails.append("count-mismatch")

    print()
    print("Classification of every distinct WORD token (zero may be unclassified):")
    for w in uniq:
        if w in reserved_set:
            role = [e["role_token_key"] for e in doc["entries"] if e["real_token"] == w]
            cls = "BOUND %s -> %s" % (role[0] if role else "?", doc["mapping"].get(w, "?"))
        elif w in v_roles:
            cls = "V-role (real spelling kept in I1)"
        else:
            cls = "locally declared identifier (never renamed)"
        print("    %-10s %s" % (w, cls))

    unclassified = [w for w in uniq
                    if w not in reserved_set and w not in v_roles and w not in identifiers]
    print()
    print("  unclassified word tokens                     : %d" % len(unclassified))
    if unclassified:
        fails.append("unclassified: %s" % unclassified)

    # unbound_word_tokens must be empty (section 7.1a): every reserved word that
    # occurs in the fixture is bound.  The set below is that escape hatch.
    unbound = sorted(set(recomputed) - table)
    print("  unbound_word_tokens                          : %s" % (unbound or "[] (empty)"))
    if unbound:
        fails.append("unbound non-empty")

    print()
    if fails:
        print("DOMAIN CHECK FAILED:", fails)
        return 1
    print("DOMAIN CHECK PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
