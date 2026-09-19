#!/usr/bin/env python3
"""
I1 keyword-map generator for the anonymized language (language_id: quidra).

Implements methodology 10 section 5 (controlled random lexicalization) exactly:
  5.1 deterministic primitives  : FNV1a32, Park-Miller next(), pick()
  5.2 pseudo-word generation    : algorithm PW-1, CVCVCV, exactly 6 ASCII lowercase chars
  5.3 assignment procedure      : per-key FNV-seeded stream, rejection loop, filters 1-7

Deterministic inputs: CONDITION="I1", LANGUAGE="quidra", SEED=20260918.
Running this file twice in separate processes must produce byte-identical mapping.json.
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

CONDITION = "I1"
LANGUAGE = "quidra"
SEED = 20260918

# ---------------------------------------------------------------- 5.1 primitives


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


# ---------------------------------------------------------------- 5.2 PW-1

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


# ---------------------------------------------------------------- word lists


def load_list(name):
    path = os.path.join(HERE, "wordlists", name)
    with open(path, "r", encoding="ascii") as fh:
        return [line.strip() for line in fh if line.strip()]


EN_COMMON = load_list("en_common.txt")
PROG_TERMS = load_list("prog_terms.txt")
RESERVED_UNION = load_list("reserved_union.txt")

REJECT_SET = set(EN_COMMON) | set(PROG_TERMS) | set(RESERVED_UNION)
REJECT_SUBSTR = sorted({w for w in REJECT_SET if len(w) >= 4})


def task_material() -> str:
    """Filter 7: the pseudo-word must appear nowhere in the task material."""
    blobs = []
    for fname in ("fixture_real.qui", "oracle_real.qui", "expected_output.txt",
                  "task_statement.txt"):
        p = os.path.join(HERE, fname)
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as fh:
                blobs.append(fh.read())
    return "\n".join(blobs)


MATERIAL = task_material()

FILTER_HITS = {str(i): 0 for i in range(1, 8)}


def accept(w, used):
    if len(w) != 6 or not all("a" <= c <= "z" for c in w):
        FILTER_HITS["1"] += 1
        return False
    if w in used:
        FILTER_HITS["2"] += 1
        return False
    if w in EN_COMMON:
        FILTER_HITS["3"] += 1
        return False
    if w in PROG_TERMS:
        FILTER_HITS["4"] += 1
        return False
    if w in RESERVED_UNION:
        FILTER_HITS["5"] += 1
        return False
    for sub in REJECT_SUBSTR:
        if sub in w:
            FILTER_HITS["6"] += 1
            return False
    if w in MATERIAL:
        FILTER_HITS["7"] += 1
        return False
    return True


# ---------------------------------------------------------------- role table
#
# role_token_key -> (real token, language-neutral role prose)
# K-role ids follow methodology 10 section 3.1; every other reserved word this
# language spells as a word and that the task's construct set can reach gets a
# "U:<token>" key (section 7.1a). Keys are sorted ascending ASCII before drawing.

ROLES = {
    "K02": ("const", "marks a binding whose value can never be written again"),
    "K04": ("if", "introduces a conditional branch"),
    "K05": ("else", "introduces the alternative branch"),
    "K06": ("elif", "introduces a chained alternative branch that carries its own condition"),
    "K07": ("for", "introduces bounded iteration over a sequence of values"),
    "K08": ("while", "introduces conditional iteration"),
    "K09": ("break", "exits the innermost loop"),
    "K10": ("continue", "proceeds to the next iteration of the innermost loop"),
    "K11": ("return", "yields a function result"),
    "K12": ("class", "introduces an aggregate type declaration"),
    "K14": ("import", "introduces an import of an external module"),
    "K16": ("true", "the boolean true literal"),
    "K17": ("false", "the boolean false literal"),
    "K18": ("and", "logical conjunction"),
    "K19": ("or", "logical disjunction"),
    "K20": ("not", "logical negation"),
    "K21a": ("int", "the 64-bit signed integer type name"),
    "K21b": ("bool", "the boolean type name"),
    "K21c": ("string", "the text type name"),
    "K21e": ("void", "the result-less designation of a function that yields nothing"),
    "K27": ("try", "propagates a failure result out of the enclosing function"),
    "U:in": ("in", "separates the loop binder from the sequence it walks"),
    "U:match": ("match", "introduces a multi-alternative dispatch on a value's type"),
    "U:override": ("override", "marks a member declaration that replaces an inherited one"),
    "U:super": ("super", "names the parent implementation inside a member declaration"),
}

NOT_LEXICALIZED = {
    "K01": "a function declaration is introduced by its result type, not by a word token",
    "K03": "a writable binding is introduced by its type, not by a word token",
    "K13": "a field declaration is introduced by its type, not by a word token",
    "K15": "this language has no compilation-unit declaration",
    "K22": "top-level statements are the entry point; no word token marks it",
    "K23": "this language has no visibility or linkage marker",
    "K24": "the mutability qualifier is the same word token as K02",
    "K25": "a type annotation is written by placing the type before the name",
    "K26": "a conversion is written as a call on the destination type name",
}

NOT_WORD_TOKEN = {
    "K21d": "the dynamic-sequence type is spelled by a bracket suffix on the element type",
}


def main():
    ordered = sorted(ROLES)
    assigned = {}
    used = set()
    redraws = {}
    for rk in ordered:
        key = "%s|%s|%d|%s" % (CONDITION, LANGUAGE, SEED, rk)
        st = seed_state(key)
        ok = False
        attempts = 0
        for _ in range(10000):
            w, st = draw_word(st)
            attempts += 1
            if accept(w, used):
                assigned[rk] = w
                used.add(w)
                ok = True
                break
        if not ok:
            sys.exit("ABORT LEXICALIZATION_EXHAUSTION " + key)
        if attempts > 1:
            redraws[rk] = attempts - 1

    # one-to-one, equal length, no prefix relation (implied by equal length + distinct)
    assert len(set(assigned.values())) == len(assigned)
    assert all(len(v) == 6 for v in assigned.values())
    for a in assigned.values():
        for b in assigned.values():
            if a != b:
                assert not a.startswith(b) and not b.startswith(a)

    def sha(name):
        with open(os.path.join(HERE, "wordlists", name), "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()

    doc = {
        "condition": CONDITION,
        "language_id": LANGUAGE,
        "seed": SEED,
        "generator": "PW-1 (methodology 10 sections 5.1-5.3)",
        "primitives": {
            "rng": "park-miller-48271",
            "hash": "fnv1a32",
            "pick_divisor": 128,
            "pattern": "CVCVCV",
            "length": 6,
        },
        "key_format": "<condition>|<language_id>|<seed>|<role_token_key>",
        "wordlist_sha256": {
            "en_common": sha("en_common.txt"),
            "prog_terms": sha("prog_terms.txt"),
            "reserved_union": sha("reserved_union.txt"),
        },
        "anonymized_token_count": len(assigned),
        "collision_redraws": redraws,
        "filter_rejection_counts": FILTER_HITS,
        "mapping": {ROLES[rk][0]: assigned[rk] for rk in ordered},
        "roles": {
            rk: {
                "status": "BOUND",
                "real_token": ROLES[rk][0],
                "pseudo_word": assigned[rk],
                "role_prose": ROLES[rk][1],
            }
            for rk in ordered
        },
        "not_lexicalized": NOT_LEXICALIZED,
        "not_word_token": NOT_WORD_TOKEN,
        "untransformed_by_design": {
            "note": "I1 transforms grammar-significant WORD tokens only. Standard-vocabulary "
                    "names (the line-writing facility, the bounded-range constructor, the "
                    "scalar-to-text member) keep their real spellings in I1 (methodology 10 "
                    "section 7.1); value interpolation lives inside a text literal and is "
                    "NOT_TRANSFORMABLE in every condition (section 3.2 V18). Ordinary user "
                    "identifiers are never renamed.",
            "names": ["print", "write", "range", "string()", "len", "append", "sorted"],
        },
    }
    out = os.path.join(HERE, "mapping.json")
    with open(out, "w", encoding="ascii") as fh:
        json.dump(doc, fh, indent=2, sort_keys=False)
        fh.write("\n")
    print("wrote %s with %d mappings" % (out, len(assigned)))
    for rk in ordered:
        print("  %-12s %-10s -> %s" % (rk, ROLES[rk][0], assigned[rk]))


if __name__ == "__main__":
    main()
