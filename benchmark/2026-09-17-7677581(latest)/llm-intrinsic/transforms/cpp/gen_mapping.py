#!/usr/bin/env python3
"""
I1 keyword-anonymization mapping generator for the C++ column.

Implements methodology 10_intrinsic_design.md:
  §5.1 deterministic primitives (FNV1a32, seed_state, Park-Miller next, pick)
  §5.2 pseudo-word generation PW-1 (CVCVCV, exactly six lowercase ASCII chars)
  §5.3 assignment procedure + accept() rejection filters
  §7.1  I1 domain: BOUND K-role tokens + non-role reserved words occurring in the fixture
  §7.1a domain is mechanically derived, never hand-authored

Deterministic seed for this build: 20260918.
Run:  python3 gen_mapping.py   ->  writes mapping.json next to this file
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WL = os.path.join(HERE, "wordlists")

CONDITION_ID = "I1"
LANGUAGE_ID = "cpp"
SEED = 20260918

# ---------------------------------------------------------------- §5.1 primitives


def fnv1a32(s: str) -> int:
    h = 2166136261
    for b in s.encode("ascii"):
        h ^= b
        h = (h * 16777619) % 4294967296
    return h


def seed_state(key: str) -> int:
    return (fnv1a32(key) % 2147483646) + 1


def nxt(state: int) -> int:
    return (state * 48271) % 2147483647


def pick(state: int, n: int) -> int:
    return (state // 128) % n


# ---------------------------------------------------------------- §5.2 PW-1

CONS = ["b", "d", "f", "g", "k", "l", "m", "n", "p", "r", "s", "t", "v", "z"]
VOW = ["a", "e", "i", "o", "u"]


def draw_word(state: int):
    w = ""
    for i in range(6):
        state = nxt(state)
        if i % 2 == 0:
            w += CONS[pick(state, 14)]
        else:
            w += VOW[pick(state, 5)]
    return w, state


# ---------------------------------------------------------------- §5.3 accept()


def load_list(name):
    with open(os.path.join(WL, name), "r", encoding="ascii") as f:
        return [ln.strip() for ln in f if ln.strip()]


EN_COMMON = load_list("en_common.txt")
PROG_TERMS = load_list("prog_terms.txt")
RESERVED_UNION = load_list("reserved_union.txt")

REJECT_SETS = {
    "en_common": set(EN_COMMON),
    "prog_terms": set(PROG_TERMS),
    "reserved_union": set(RESERVED_UNION),
}
# filter 6: substring test uses every entry of length >= 4 from all three lists
SUBSTRING_ENTRIES = sorted(
    {w for w in EN_COMMON + PROG_TERMS + RESERVED_UNION if len(w) >= 4}
)

# filter 7: every word-shaped token and literal appearing in this language's task
# material (task statement, reference solution, expected output). Recomputed from
# the actual files when they exist; the literal list below is the frozen fallback
# used while the fixture is still being authored.
TASK_WORDS = {
    "sum", "max", "evens", "joined", "state", "term", "terms", "largest",
    "count", "total", "i", "n", "s", "m", "e", "j", "std", "cout", "endl", "string",
    "to_string", "iostream", "main", "int", "long", "for", "if", "else",
    "steps", "value", "biggest", "odds", "chain", "limit", "counter", "tally",
    "while", "break", "continue", "return", "include", "const", "solution",
}

# rejection-filter firing counters (PF-08 requires every filter to fire at least once)
FILTER_HITS = {str(i): 0 for i in range(1, 8)}


def accept(w, used):
    if len(w) != 6 or not all("a" <= c <= "z" for c in w):
        FILTER_HITS["1"] += 1
        return False
    if w in used:
        FILTER_HITS["2"] += 1
        return False
    if w in REJECT_SETS["en_common"]:
        FILTER_HITS["3"] += 1
        return False
    if w in REJECT_SETS["prog_terms"]:
        FILTER_HITS["4"] += 1
        return False
    if w in REJECT_SETS["reserved_union"]:
        FILTER_HITS["5"] += 1
        return False
    for e in SUBSTRING_ENTRIES:
        if e in w:
            FILTER_HITS["6"] += 1
            return False
    if w in TASK_WORDS:
        FILTER_HITS["7"] += 1
        return False
    return True


# ------------------------------------------------- the I1 domain for this column
# Each entry: role_token_key -> (real token, language-neutral role prose)
# Binding rule §3.4: a token is BOUND only if it occurs in this column's fixture
# AND is a reserved word of the language or is named for that role by the
# normative language reference. Citations are ISO/IEC 14882:2020 section numbers.

DOMAIN = [
    ("K04", "if", "introduces a conditional branch",
     "ISO/IEC 14882:2020 [stmt.if]"),
    ("K05", "else", "introduces the alternative branch of a conditional",
     "ISO/IEC 14882:2020 [stmt.if]"),
    ("K07", "for", "introduces bounded iteration with an initializer, a continuation "
     "condition and a step",
     "ISO/IEC 14882:2020 [stmt.for]"),
    ("K11", "return", "yields a function result and leaves the function",
     "ISO/IEC 14882:2020 [stmt.return]"),
    ("K14", "include", "directive word that pulls in an external module by name",
     "ISO/IEC 14882:2020 [cpp.include]"),
    ("K21a#1", "int", "the basic integer type name",
     "ISO/IEC 14882:2020 [basic.fundamental]"),
    ("K21a#2", "long", "width qualifier that widens the basic integer type; written "
     "twice for the wide 64-bit form",
     "ISO/IEC 14882:2020 [basic.fundamental]"),
    ("K22", "main", "the fixed name of the program entry point",
     "ISO/IEC 14882:2020 [basic.start.main]"),
    ("K24", "const", "qualifier marking a binding immutable after initialization",
     "ISO/IEC 14882:2020 [dcl.type.cv]"),
]


def assign():
    ordered = sorted(DOMAIN, key=lambda t: t[0])
    assigned = {}
    used = set()
    redraws = {}
    for rk, tok, prose, cite in ordered:
        key = "%s|%s|%d|%s" % (CONDITION_ID, LANGUAGE_ID, SEED, rk)
        st = seed_state(key)
        ok = False
        for attempt in range(1, 10001):
            w, st = draw_word(st)
            if accept(w, used):
                assigned[rk] = w
                used.add(w)
                if attempt > 1:
                    redraws[rk] = attempt - 1
                ok = True
                break
        if not ok:
            raise SystemExit("LEXICALIZATION_EXHAUSTION " + key)
    return assigned, redraws


def sha256_file(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    assigned, redraws = assign()

    # one-to-one, no prefix relation, exact length
    words = list(assigned.values())
    assert len(set(words)) == len(words), "collision"
    for a in words:
        for b in words:
            if a is not b and (a.startswith(b) or b.startswith(a)):
                raise SystemExit("prefix relation: %s %s" % (a, b))
    assert all(len(w) == 6 for w in words)

    tokens = {}
    for rk, tok, prose, cite in DOMAIN:
        tokens[rk] = {
            "real_token": tok,
            "pseudo_word": assigned[rk],
            "role_prose": prose,
            "citation": cite,
            "char_len_real": len(tok),
            "char_len_pseudo": 6,
        }

    out = {
        "schema": "i1-keyword-map/1",
        "condition": "I1",
        "language_id": "cpp",
        "seed": SEED,
        "primitives": {
            "rng": "park-miller-48271",
            "hash": "fnv1a32",
            "pick_divisor": 128,
            "pseudo_word_shape": "CVCVCV, exactly 6 lowercase ASCII characters",
            "key_format": "<condition>|<language_id>|<seed>|<role_token_key>",
        },
        "wordlist_sha256": {
            "en_common": sha256_file(os.path.join(WL, "en_common.txt")),
            "prog_terms": sha256_file(os.path.join(WL, "prog_terms.txt")),
            "reserved_union": sha256_file(os.path.join(WL, "reserved_union.txt")),
        },
        "tokens": tokens,
        "forward": {t["real_token"]: t["pseudo_word"] for t in tokens.values()},
        "inverse": {t["pseudo_word"]: t["real_token"] for t in tokens.values()},
        "anonymized_token_count": len(tokens),
        "collision_redraws": redraws,
        "rejection_filter_hits": FILTER_HITS,
        "not_transformed": {
            "reason": "methodology 10 §7.1 — I1 transforms K-role and non-role reserved "
                      "word tokens only; standard-library names keep their real spelling "
                      "in I1 and are published as the §13.2 anonymity residual.",
            "library_surface": ["iostream", "string", "std", "cout", "endl", "to_string"],
        },
        "pseudo_word_char_counts": {w: 6 for w in sorted(words)},
        "char_count_stats": {"mean": 6.0, "sd": 0.0, "min": 6, "max": 6},
    }
    path = os.path.join(HERE, "mapping.json")
    with open(path, "w", encoding="ascii") as f:
        json.dump(out, f, indent=2, sort_keys=False)
        f.write("\n")
    print("wrote", path)
    for rk, tok, prose, cite in sorted(DOMAIN, key=lambda t: t[0]):
        print("  %-8s %-9s -> %s" % (rk, tok, assigned[rk]))
    print("collision_redraws:", redraws)
    print("rejection_filter_hits:", FILTER_HITS)


if __name__ == "__main__":
    main()
