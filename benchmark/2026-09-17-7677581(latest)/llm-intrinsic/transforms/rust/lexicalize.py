#!/usr/bin/env python3
"""I1 controlled random lexicalization for the 'rust' column.

Implements methodology 10_intrinsic_design.md sections 5.1 (deterministic
primitives), 5.2 (algorithm PW-1) and 5.3 (assignment procedure) exactly.

Usage:  python3 lexicalize.py [--out mapping.json]

Everything is a pure function of the frozen seed, the frozen role-token keys
and the three frozen word lists.  Two separate processes must produce
byte-identical output.
"""
import json
import os
import sys
import hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
WL = os.path.join(HERE, "wordlists")

CONDITION_ID = "I1"
LANGUAGE_ID = "rust"
SEED = 20260918

# ---------------------------------------------------------------- 5.1 primitives

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


# ---------------------------------------------------------------- 5.2 PW-1

CONS = ["b", "d", "f", "g", "k", "l", "m", "n", "p", "r", "s", "t", "v", "z"]
VOW = ["a", "e", "i", "o", "u"]


def draw_word(state):
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
    with open(os.path.join(WL, name), "r", encoding="ascii") as f:
        return set(line.strip() for line in f if line.strip())


EN_COMMON = load_list("en_common.txt")
PROG_TERMS = load_list("prog_terms.txt")
RESERVED_UNION = load_list("reserved_union.txt")
# entries of length >= 4 from lists (3)-(5), used by accept() rule 6
SUBSTR_BAN = set(w for w in (EN_COMMON | PROG_TERMS | RESERVED_UNION) if len(w) >= 4)

# accept() rule 7: the pseudo-word must not occur anywhere in the task material
TASK_MATERIAL_FILES = ["fixture_real.rs", "task_statement.txt", "expected_output.txt"]


def task_material():
    blob = ""
    for fn in TASK_MATERIAL_FILES:
        p = os.path.join(HERE, fn)
        if os.path.exists(p):
            blob += open(p, "r", encoding="utf-8").read() + "\n"
    return blob


TASK_BLOB = task_material()

REJECTION_COUNTS = {"rule1": 0, "rule2": 0, "rule3": 0, "rule4": 0,
                    "rule5": 0, "rule6": 0, "rule7": 0}


def accept(w, used):
    import re
    if not re.match(r"^[a-z]{6}$", w):
        REJECTION_COUNTS["rule1"] += 1
        return False
    if w in used:
        REJECTION_COUNTS["rule2"] += 1
        return False
    if w in EN_COMMON:
        REJECTION_COUNTS["rule3"] += 1
        return False
    if w in PROG_TERMS:
        REJECTION_COUNTS["rule4"] += 1
        return False
    if w in RESERVED_UNION:
        REJECTION_COUNTS["rule5"] += 1
        return False
    for ln in (4, 5, 6):
        for st in range(0, 6 - ln + 1):
            if w[st:st + ln] in SUBSTR_BAN:
                REJECTION_COUNTS["rule6"] += 1
                return False
    if w in TASK_BLOB:
        REJECTION_COUNTS["rule7"] += 1
        return False
    return True


# ---------------------------------------------------------------- 5.3 assignment

# Role token keys -> the real token they bind, with the normative citation.
# Binding rule of methodology 3.4: a token is BOUND to a role only if it occurs
# in the reference solution / pack examples AND is named for that role in the
# language's normative reference or reserved-word list.
ROLES = {
    "K01": {"token": "fn",
            "role_prose": "introduces a function definition",
            "citation": "normative reserved-word list, entry 'fn'; language reference, Items / Functions"},
    "K02": {"token": "let",
            "role_prose": "introduces a named local value binding",
            "citation": "normative reserved-word list, entry 'let'; language reference, Statements / Let statements"},
    "K04": {"token": "if",
            "role_prose": "introduces a conditional branch",
            "citation": "normative reserved-word list, entry 'if'; language reference, Expressions / If expressions"},
    "K05": {"token": "else",
            "role_prose": "introduces the alternative branch of a conditional",
            "citation": "normative reserved-word list, entry 'else'; language reference, Expressions / If expressions"},
    "K08": {"token": "while",
            "role_prose": "introduces conditional iteration",
            "citation": "normative reserved-word list, entry 'while'; language reference, Expressions / Loop expressions"},
    "K21a": {"token": "i64",
             "role_prose": "the 64-bit signed integer type name used by the task",
             "citation": "language reference, Types / Numeric types (machine-dependent and sized integer types)"},
    "K21c": {"token": "String",
             "role_prose": "the growable text type name",
             "citation": "standard library reference, owned growable text type"},
    "K22": {"token": "main",
            "role_prose": "the fixed name of the program entry point",
            "citation": "language reference, Crates and source files / the main function"},
    "K24": {"token": "mut",
            "role_prose": "mutability qualifier applied to a local binding",
            "citation": "normative reserved-word list, entry 'mut'; language reference, Statements / Let statements"},
}

NOT_LEXICALIZED = {
    "K03": "the mutable binding form is spelled as K02 followed by K24, not as a distinct word",
    "K06": "a chained alternative is spelled as K05 followed by K04, not as a distinct word",
    "K07": "bounded iteration is not used by this task; conditional iteration (K08) covers it",
    "K11": "not required: the entry point yields no result and the task defines no other function",
    "K12": "no aggregate type is required by this task",
    "K13": "no aggregate type is required by this task",
    "K14": "no import is required: the facilities this task needs are in the automatic prelude",
    "K15": "the compilation unit needs no module/package declaration",
    "K16": "no boolean literal is required by this task",
    "K17": "no boolean literal is required by this task",
    "K18": "logical conjunction is spelled as an operator, not a word",
    "K19": "logical disjunction is spelled as an operator, not a word",
    "K20": "logical negation is spelled as an operator, not a word",
    "K21b": "no boolean type annotation is required by this task",
    "K21d": "no dynamic-sequence type is required by this task",
    "K21e": "the result-less designation is the absence of a result clause, not a word",
    "K23": "the entry point needs no visibility marker",
    "K25": "a type annotation is introduced by punctuation, not a word",
    "K26": "no type conversion is required by this task",
    "K27": "the output facility this task uses propagates no error, so no marker is required",
}


def assign():
    ordered = sorted(ROLES.keys())          # ascending ASCII order of role token key
    assigned = {}
    used = set()
    redraws = {}
    for rk in ordered:
        key = "%s|%s|%d|%s" % (CONDITION_ID, LANGUAGE_ID, SEED, rk)
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
            sys.stderr.write("ABORT LEXICALIZATION_EXHAUSTION %s\n" % key)
            sys.exit(2)
        if attempts > 1:
            redraws[rk] = attempts - 1
    return assigned, redraws


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    assigned, redraws = assign()
    forward_map = {}
    for rk, pw in assigned.items():
        forward_map[ROLES[rk]["token"]] = pw

    # one-to-one and no-prefix verification, inline
    pws = sorted(forward_map.values())
    assert len(set(pws)) == len(pws), "pseudo-words are not distinct"
    for a in pws:
        for b in pws:
            if a != b:
                assert not a.startswith(b), "prefix relation %s / %s" % (a, b)
    assert len(set(forward_map.keys())) == len(forward_map), "real tokens not distinct"

    doc = {
        "schema": "quidra-benchmark/I1-keyword-map/1",
        "condition": "I1",
        "subtest": "I1 Keyword Anonymization",
        "language_id": LANGUAGE_ID,
        "deterministic_seed": SEED,
        "primitives": {"rng": "park-miller-48271", "hash": "fnv1a32",
                       "pick_divisor": 128, "pseudo_word_shape": "CVCVCV, exactly 6 lowercase ASCII"},
        "key_format": "<condition_id>|<language_id>|<decimal seed>|<role_token_key>",
        "wordlist_sha256": {
            "en_common": sha256_file(os.path.join(WL, "en_common.txt")),
            "prog_terms": sha256_file(os.path.join(WL, "prog_terms.txt")),
            "reserved_union": sha256_file(os.path.join(WL, "reserved_union.txt")),
            "reserved_rust": sha256_file(os.path.join(WL, "reserved_rust.txt")),
        },
        "roles": {},
        "not_lexicalized": NOT_LEXICALIZED,
        "forward": forward_map,
        "inverse": dict((v, k) for k, v in forward_map.items()),
        "collision_redraws": redraws,
        "anonymized_token_count": len(forward_map),
        "untransformed_by_design": {
            "note": "I1 transforms grammar-significant word tokens only. Standard-library "
                    "and builtin names (V-roles) keep their real spellings in I1 by "
                    "methodology section 7.1, and are therefore NOT in this map.",
            "tokens": ["println", "format", "new"]
        },
        "rejection_counts": REJECTION_COUNTS,
    }
    for rk in sorted(ROLES):
        doc["roles"][rk] = {
            "status": "BOUND",
            "token": ROLES[rk]["token"],
            "pseudo_word": assigned[rk],
            "role_prose": ROLES[rk]["role_prose"],
            "citation": ROLES[rk]["citation"],
            "key": "%s|%s|%d|%s" % (CONDITION_ID, LANGUAGE_ID, SEED, rk),
        }

    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(HERE, "mapping.json")
    with open(out, "w", encoding="ascii") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")
    sys.stderr.write("wrote %s\n" % out)


if __name__ == "__main__":
    main()
