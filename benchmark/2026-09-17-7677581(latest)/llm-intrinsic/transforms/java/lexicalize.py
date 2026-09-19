#!/usr/bin/env python3
"""I1 controlled random lexicalization for language_id = java, seed 20260918.

Implements methodology 10 section 5 (spec 10.3) exactly:
  - FNV1a32 / seed_state / Park-Miller next / pick  (5.1)
  - PW-1 draw_word: six ASCII lowercase characters, pattern C V C V C V  (5.2)
  - assign(): per-key FNV-seeded stream, ascending ASCII order of role token key,
    rejection loop with accept()  (5.3)

Deterministic: running this twice in separate processes produces byte-identical
mapping.json (verified at preflight check (c) / PF-08).
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WL = os.path.join(HERE, "wordlists")

CONDITION_ID = "I1"
LANGUAGE_ID = "java"
SEED = 20260918

CONS = ["b", "d", "f", "g", "k", "l", "m", "n", "p", "r", "s", "t", "v", "z"]
VOW = ["a", "e", "i", "o", "u"]

# The I1 domain (methodology 10 section 7.1a): every token of this language's
# frozen reserved-word list that actually occurs in the fixture.  Each entry is
# (role_token_key, real_token, language-neutral role prose).
DOMAIN = [
    ("K04", "if", "introduces a conditional branch; the condition follows in round brackets"),
    ("K05", "else", "introduces the alternative branch of a conditional"),
    ("K07", "for", "introduces bounded iteration with an initialiser, a condition and a step"),
    ("K12", "class", "introduces the compilation unit's named enclosing type"),
    ("K21a", "int", "the 32-bit signed integer type name"),
    ("K21e", "void", "the result-less designation of a procedure"),
    ("K23", "public", "visibility marker required on the enclosing type and on the entry point"),
    ("U:long", "long", "the 64-bit signed integer type name"),
    ("U:static", "static",
     "marks a member as belonging to the enclosing type itself rather than to an instance; "
     "required on the entry point"),
]


def fnv1a32(s):
    h = 2166136261
    for b in s.encode("ascii"):
        h ^= b
        h = (h * 16777619) % 4294967296
    return h


def seed_state(key):
    return (fnv1a32(key) % 2147483646) + 1


def nxt(state):
    return (state * 48271) % 2147483647


def pick(state, n):
    return (state // 128) % n


def draw_word(state):
    w = ""
    for i in range(6):
        state = nxt(state)
        if i % 2 == 0:
            w += CONS[pick(state, 14)]
        else:
            w += VOW[pick(state, 5)]
    return w, state


def load_list(name):
    path = os.path.join(WL, name)
    with open(path, encoding="ascii") as f:
        return [ln.strip() for ln in f if ln.strip()]


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def task_material():
    """Every byte of task material a pseudo-word must not occur in (accept rule 7)."""
    blobs = []
    for name in ("fixture_real.java", "expected_output.txt", "task_statement.txt"):
        p = os.path.join(HERE, name)
        if os.path.exists(p):
            blobs.append(open(p, encoding="utf-8").read())
    return "\n".join(blobs).lower()


def build_accept():
    en = set(load_list("en_common.txt"))
    pt = set(load_list("prog_terms.txt"))
    ru = set(load_list("reserved_union.txt"))
    rj = set(load_list("reserved_java.txt"))
    substr = sorted({w for w in (en | pt | ru | rj) if len(w) >= 4})
    material = task_material()
    fired = {str(k): 0 for k in range(1, 8)}

    def accept(w, used):
        import re

        if not re.match(r"^[a-z]{6}$", w):
            fired["1"] += 1
            return False
        if w in used:
            fired["2"] += 1
            return False
        if w in en:
            fired["3"] += 1
            return False
        if w in pt:
            fired["4"] += 1
            return False
        if w in ru or w in rj:
            fired["5"] += 1
            return False
        for s in substr:
            if s in w:
                fired["6"] += 1
                return False
        if w in material:
            fired["7"] += 1
            return False
        return True

    return accept, fired, {
        "en_common.txt": sha256_file(os.path.join(WL, "en_common.txt")),
        "prog_terms.txt": sha256_file(os.path.join(WL, "prog_terms.txt")),
        "reserved_union.txt": sha256_file(os.path.join(WL, "reserved_union.txt")),
        "reserved_java.txt": sha256_file(os.path.join(WL, "reserved_java.txt")),
    }


def assign():
    accept, fired, hashes = build_accept()
    ordered = sorted(DOMAIN, key=lambda e: e[0])  # ascending ASCII order of role token key
    assigned = {}
    used = set()
    redraws = {}
    for rk, token, _prose in ordered:
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
            raise SystemExit("ABORT LEXICALIZATION_EXHAUSTION " + key)
    return assigned, redraws, fired, hashes


def prefix_free(words):
    for a in words:
        for b in words:
            if a is not b and (a.startswith(b) or b.startswith(a)):
                return False
    return True


def main():
    assigned, redraws, fired, hashes = assign()
    words = [assigned[rk] for rk, _, _ in DOMAIN]
    assert len(set(words)) == len(words), "pseudo-words are not one-to-one"
    assert all(len(w) == 6 for w in words)
    assert prefix_free(words), "prefix relation between pseudo-words"

    mapping = {}
    rows = []
    for rk, token, prose in sorted(DOMAIN, key=lambda e: e[0]):
        mapping[token] = assigned[rk]
        rows.append({
            "role_token_key": rk,
            "real_token": token,
            "pseudo_word": assigned[rk],
            "role_prose": prose,
            "char_len_real": len(token),
            "char_len_pseudo": len(assigned[rk]),
            "collision_redraws": redraws.get(rk, 0),
        })

    doc = {
        "unit": "I1/java",
        "condition": "I1",
        "language_id": LANGUAGE_ID,
        "seed": SEED,
        "generator": os.path.basename(__file__),
        "generator_sha256": sha256_file(os.path.abspath(__file__)),
        "primitives": {
            "rng": "park-miller-48271",
            "hash": "fnv1a32",
            "pick_divisor": 128,
            "pseudo_word_shape": "CVCVCV, exactly 6 ASCII lowercase characters",
            "key_format": "<condition_id>|<language_id>|<seed>|<role_token_key>",
        },
        "wordlist_sha256": hashes,
        "domain_rule": (
            "every token of wordlists/reserved_java.txt that occurs as a whole WORD token in "
            "fixture_real.java (methodology 10 section 7.1a)"
        ),
        "not_transformed": {
            "V-roles": [
                "System", "out", "println", "String", "args", "Main",
            ],
            "reason": (
                "I1 transforms grammar-significant word tokens only; standard-vocabulary names "
                "keep their real spellings in I1 (methodology 10 section 7.1). Ordinary user "
                "identifiers (state, sum, max, evens, joined, term, i) are never renamed."
            ),
        },
        "anonymized_token_count": len(rows),
        "mapping": mapping,
        "inverse_mapping": {v: k for k, v in mapping.items()},
        "entries": rows,
        "collision_redraws": redraws,
        "rejection_filters_fired": fired,
        "invariants": {
            "one_to_one": True,
            "all_six_chars": True,
            "prefix_free": True,
            "char_length_mean": 6.0,
            "char_length_sd": 0.0,
            "char_length_min": 6,
            "char_length_max": 6,
        },
    }

    out = os.path.join(HERE, "mapping.json")
    with open(out, "w", encoding="ascii") as f:
        json.dump(doc, f, indent=2, sort_keys=False)
        f.write("\n")
    print("wrote", out)
    for r in rows:
        print("  %-10s %-8s -> %s" % (r["role_token_key"], r["real_token"], r["pseudo_word"]))
    print("rejection filters fired:", fired)


if __name__ == "__main__":
    sys.exit(main())
