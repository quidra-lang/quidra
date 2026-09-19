#!/usr/bin/env python3
"""
I1 keyword-map generator for language_id = "go".

Implements methodology 10, section 5 (Controlled random lexicalization) exactly:
  - 5.1 deterministic primitives  (FNV1a32, seed_state, Park-Miller next, pick)
  - 5.2 algorithm PW-1            (draw_word: always 6 chars, CVCVCV)
  - 5.3 assignment procedure      (per-key FNV-seeded stream + accept() rejection loop)

Nothing here is random at run time: given the seed and the frozen word lists the
output is byte-identical in every process, on every machine.
"""

import hashlib
import json
import os
import sys
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))

CONDITION_ID = "I1"
LANGUAGE_ID = "go"
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
    with open(path, "r", encoding="utf-8") as fh:
        return set(line.strip() for line in fh if line.strip())


EN_COMMON = load_list("en_common.txt")
PROG_TERMS = load_list("prog_terms.txt")
RESERVED_UNION = load_list("reserved_union.txt")

# accept() filter 6: no entry of length >= 4 from lists (3)-(5) may be a
# contiguous substring of the pseudo-word.
SUBSTR_BAN = set(
    w for w in (EN_COMMON | PROG_TERMS | RESERVED_UNION) if 4 <= len(w) <= 6
)


def _banned_substring(w):
    """Longest-first: report the longest banned contiguous substring of w, or None.

    A pseudo-word is always 6 characters, so only substrings of length 4, 5 and 6
    can be an entry of length >= 4.  This is exactly filter (6) of section 5.3,
    evaluated by lookup instead of by scanning the list.
    """
    for ln in (6, 5, 4):
        for i in range(0, 6 - ln + 1):
            s = w[i:i + ln]
            if s in SUBSTR_BAN:
                return s
    return None

# accept() filter 7: nothing that occurs anywhere in the task material.
# The task material for this single-task build is the task statement, the real
# fixture and the expected output.  Every word-shaped token in them is banned.
TASK_MATERIAL_WORDS = {
    # task statement / expected output
    "sum", "max", "evens", "joined", "park", "miller", "minimal", "standard",
    "lcg", "state", "term", "terms", "hyphens", "status",
    # fixture identifiers, literals and library names
    "package", "main", "import", "fmt", "strconv", "func", "var", "int64",
    "string", "for", "if", "println", "formatint", "largest", "i",
}


def accept(w, used):
    if len(w) != 6 or not all("a" <= c <= "z" for c in w):
        return False, "shape"
    if w in used:
        return False, "used"
    if w in EN_COMMON:
        return False, "en_common"
    if w in PROG_TERMS:
        return False, "prog_terms"
    if w in RESERVED_UNION:
        return False, "reserved_union"
    b = _banned_substring(w)
    if b is not None:
        return False, "substring:" + b
    if w in TASK_MATERIAL_WORDS:
        return False, "task_material"
    return True, None


# ---------------------------------------------------------------- 5.3 assignment

# The I1 lexical domain for this language and this task, per methodology 10
# section 7.1a: every grammar-significant word token that actually occurs in the
# fixture.  V-roles (fmt, Println, strconv, FormatInt) are NOT transformed in I1
# (section 7.1) and are therefore absent from this table.
ROLES = {
    "K01": ("func", "introduces a function definition"),
    "K02": ("var", "introduces a named value binding"),
    "K04": ("if", "introduces a conditional branch"),
    "K07": ("for", "introduces bounded iteration"),
    "K14": ("import", "introduces an import of an external module"),
    "K15": ("package", "declares the compilation unit's module"),
    "K21a": ("int64", "the 64-bit signed integer type name"),
    "K21c": ("string", "the text type name"),
    "K22": ("main", "the toolchain-fixed name of the module that holds the "
                    "entry point and of the entry-point function itself"),
}


def assign():
    ordered = sorted(ROLES.keys())  # ascending ASCII order of the role token key
    assigned = {}
    used = set()
    redraws = {}
    filters_fired = {}
    for rk in ordered:
        key = "%s|%s|%d|%s" % (CONDITION_ID, LANGUAGE_ID, SEED, rk)
        st = seed_state(key)
        ok = False
        attempts = 0
        for _ in range(10000):
            w, st = draw_word(st)
            attempts += 1
            good, why = accept(w, used)
            if good:
                assigned[rk] = w
                used.add(w)
                ok = True
                break
            fam = why.split(":")[0]
            filters_fired[fam] = filters_fired.get(fam, 0) + 1
        if not ok:
            raise SystemExit("LEXICALIZATION_EXHAUSTION " + key)
        redraws[rk] = attempts - 1
    return assigned, redraws, filters_fired


def sha256_file(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def main():
    assigned, redraws, filters_fired = assign()

    # one-to-one, no prefix relation, exact length 6
    words = list(assigned.values())
    assert len(set(words)) == len(words), "collision"
    for a in words:
        for b in words:
            if a is not b and (a.startswith(b) or b.startswith(a)):
                raise SystemExit("prefix relation: %s %s" % (a, b))
        assert len(a) == 6

    out = {
        "condition": CONDITION_ID,
        "language_id": LANGUAGE_ID,
        "seed": SEED,
        "generated_at": datetime.datetime.now(datetime.timezone.utc)
        .replace(microsecond=0)
        .isoformat(),
        "generator": "PW-1 / methodology 10 sections 5.1-5.3",
        "primitives": {
            "rng": "park-miller-48271",
            "hash": "fnv1a32",
            "pick_divisor": 128,
            "pattern": "CVCVCV, exactly 6 lowercase ASCII characters",
            "key_format": "<condition>|<language_id>|<seed>|<role_token_key>",
        },
        "wordlist_sha256": {
            "en_common": sha256_file(os.path.join(HERE, "wordlists/en_common.txt")),
            "prog_terms": sha256_file(os.path.join(HERE, "wordlists/prog_terms.txt")),
            "reserved_union": sha256_file(
                os.path.join(HERE, "wordlists/reserved_union.txt")
            ),
        },
        "not_transformed_in_I1": {
            "note": "methodology 10 section 7.1: V-roles keep their real spelling in I1; "
                    "ordinary user identifiers are never renamed.",
            "v_roles": {
                "V01": "fmt.Println",
                "V10": "strconv.FormatInt",
                "V16": "fmt, strconv",
            },
            "user_identifiers": [
                "state", "sum", "largest", "evens", "joined", "i", "term"
            ],
        },
        "mapping": {},
        "reverse": {},
        "collision_redraws": redraws,
        "rejection_filters_fired": filters_fired,
        "anonymized_token_count": len(assigned),
    }
    for rk in sorted(assigned):
        real, prose = ROLES[rk]
        out["mapping"][rk] = {
            "real": real,
            "pseudo": assigned[rk],
            "role_prose": prose,
            "chars": 6,
        }
        out["reverse"][assigned[rk]] = real

    with open(os.path.join(HERE, "mapping.json"), "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=2, sort_keys=False)
        fh.write("\n")

    for rk in sorted(assigned):
        print("%-5s %-8s -> %s   (redraws %d)" % (rk, ROLES[rk][0], assigned[rk], redraws[rk]))
    print("filters fired:", filters_fired)


if __name__ == "__main__":
    main()
