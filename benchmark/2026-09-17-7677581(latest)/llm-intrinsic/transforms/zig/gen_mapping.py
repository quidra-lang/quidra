#!/usr/bin/env python3
"""
Controlled random lexicalization for I1 / language_id = "zig".
Implements methodology 10 sections 5.1 (primitives), 5.2 (PW-1) and 5.3 (assignment)
verbatim. Deterministic: same seed -> same mapping, in any process, on any machine.

Usage:  python3 gen_mapping.py [--out mapping.json] [--print]
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

CONDITION = "I1"
LANGUAGE_ID = "zig"
SEED = 20260918

# ----------------------------------------------------------------- 5.1 primitives


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


# ----------------------------------------------------------------- 5.2 PW-1

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


# ----------------------------------------------------------------- word lists


def load_list(name):
    path = os.path.join(HERE, "wordlists", name)
    with open(path, "r", encoding="utf-8") as fh:
        return set(x.strip() for x in fh if x.strip())


def sha256_file(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


EN_COMMON = load_list("en_common.txt")
PROG_TERMS = load_list("prog_terms.txt")
RESERVED_UNION = load_list("reserved_union.txt")
RESERVED_ZIG = load_list("reserved_zig.txt")

# filter 6 operates on every entry of length >= 4 in lists (3)-(5).
# reserved_zig is folded into the union here so that no pseudo-word can be, or
# contain, a reserved word of THIS language either -- reserved_union was frozen
# before this language's list existed, so the containment is checked, not assumed.
UNION_ALL = RESERVED_UNION | RESERVED_ZIG
SUBSTRING_BAN = sorted(
    w for w in (EN_COMMON | PROG_TERMS | UNION_ALL) if len(w) >= 4
)

WORD6 = re.compile(r"^[a-z]{6}$")

# filter 7: the pseudo-word must not occur anywhere in the task material.
TASK_MATERIAL_FILES = ["fixture_real.zig", "expected_output.txt", "task_statement.txt"]


def task_material():
    blob = []
    for name in TASK_MATERIAL_FILES:
        p = os.path.join(HERE, name)
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as fh:
                blob.append(fh.read())
    return "\n".join(blob)


TASK_BLOB = task_material()

FILTERS_FIRED = {
    "not_word6": 0,
    "already_used": 0,
    "en_common": 0,
    "prog_terms": 0,
    "reserved_union": 0,
    "substring": 0,
    "task_material": 0,
}


def reject_reasons(w, used):
    """Every filter that would reject w, with NO short-circuit.

    accept() below short-circuits (that is what produces the honest
    rejection_filters_fired tally), which makes it unusable for demonstrating
    that each individual filter can fire.  This function exists purely so that
    PF-08 / PF-05 can exercise every filter in isolation.  The two always agree
    on the boolean: accept(w, used) == (reject_reasons(w, used) == []).
    """
    r = []
    if not WORD6.match(w):
        r.append("not_word6")
    if w in used:
        r.append("already_used")
    if w in EN_COMMON:
        r.append("en_common")
    if w in PROG_TERMS:
        r.append("prog_terms")
    if w in UNION_ALL:
        r.append("reserved_union")
    if any(b in w for b in SUBSTRING_BAN):
        r.append("substring")
    if w in TASK_BLOB:
        r.append("task_material")
    return r


def accept(w, used):
    if not WORD6.match(w):
        FILTERS_FIRED["not_word6"] += 1
        return False
    if w in used:
        FILTERS_FIRED["already_used"] += 1
        return False
    if w in EN_COMMON:
        FILTERS_FIRED["en_common"] += 1
        return False
    if w in PROG_TERMS:
        FILTERS_FIRED["prog_terms"] += 1
        return False
    if w in UNION_ALL:
        FILTERS_FIRED["reserved_union"] += 1
        return False
    for b in SUBSTRING_BAN:
        if b in w:
            FILTERS_FIRED["substring"] += 1
            return False
    if w in TASK_BLOB:
        FILTERS_FIRED["task_material"] += 1
        return False
    return True


# ----------------------------------------------------------------- the I1 domain
#
# Methodology 10 section 7.1 + 7.1a:
#   every BOUND K-role token, AND every non-role reserved word that occurs in this
#   language's fixture.  Non-role reserved words take the role token key "U:<token>".
#   V-roles (standard-library / builtin names) are NOT transformed in I1.
#
# The K-role bindings below are the ones this task's constructs actually need; the
# "U:" entries are recomputed mechanically at validation time as
#   (reserved_zig.txt  INTERSECT  fixture word tokens)  MINUS  (K-role real tokens)
# so that the table cannot silently drift from the fixture.

ROLES = {
    "K01": ("fn", "introduces a function definition"),
    "K02": ("const", "introduces a named value binding that may not be reassigned"),
    "K03": ("var", "introduces a named value binding that may be reassigned"),
    "K04": ("if", "introduces a conditional branch"),
    "K08": ("while", "introduces conditional iteration"),
    "K21a": ("u64", "the unsigned 64-bit integer type name used by the task"),
    "K21e": ("void", "the result-less designation of a function that yields no value"),
    "K22": ("main", "the toolchain-fixed name of the program entry point"),
    "K23": ("pub", "visibility marker required on the entry point declaration"),
    "K27": ("try", "error-propagation marker: applied to a call that may fail, it "
                   "forwards the failure out of the enclosing function"),
}

UNROLED = {
    "U:u8": ("u8", "the unsigned 8-bit integer (byte) type name"),
    "U:undefined": ("undefined", "the explicitly-uninitialized initial value of a "
                                 "binding whose contents are written before they are read"),
}

# V-roles -- documented as NOT transformed in I1 (section 7.1)
V_ROLES_NOT_TRANSFORMED = {
    "V01": "std.Io.File.stdout().writer(...).interface.print",
    "V16": "std (reached through the @import builtin)",
    "V17": "std.Io.Threaded / .init_single_threaded (the Io instance the output path requires)",
    "V18": "{d} inside a text literal (NOT_TRANSFORMABLE: its syntax lies inside a text literal)",
}


def assign():
    roles = {}
    roles.update({k: v for k, v in ROLES.items()})
    roles.update({k: v for k, v in UNROLED.items()})

    ordered = sorted(roles.keys())  # ascending ASCII order of role token key
    assigned = {}
    used = set()
    redraws = {}
    for rk in ordered:
        key = "%s|%s|%d|%s" % (CONDITION, LANGUAGE_ID, SEED, rk)
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
            raise SystemExit("LEXICALIZATION_EXHAUSTION %s" % key)
        redraws[rk] = attempts - 1
    return assigned, redraws


def build(now=None):
    assigned, redraws = assign()
    roles = {}
    roles.update(ROLES)
    roles.update(UNROLED)

    mapping = {}
    reverse = {}
    for rk in sorted(assigned):
        real, prose = roles[rk]
        pw = assigned[rk]
        mapping[rk] = {"real": real, "pseudo": pw, "role_prose": prose, "chars": len(pw)}
        reverse[pw] = real

    doc = {
        "condition": CONDITION,
        "language_id": LANGUAGE_ID,
        "seed": SEED,
        "generated_at": now or datetime.datetime.now(datetime.timezone.utc)
        .replace(microsecond=0).isoformat(),
        "generator": "PW-1 / methodology 10 sections 5.1-5.3",
        "primitives": {
            "rng": "park-miller-48271",
            "hash": "fnv1a32",
            "pick_divisor": 128,
            "pattern": "CVCVCV, exactly 6 lowercase ASCII characters",
            "key_format": "<condition>|<language_id>|<seed>|<role_token_key>",
        },
        "wordlist_sha256": {
            "en_common": sha256_file(os.path.join(HERE, "wordlists", "en_common.txt")),
            "prog_terms": sha256_file(os.path.join(HERE, "wordlists", "prog_terms.txt")),
            "reserved_union": sha256_file(
                os.path.join(HERE, "wordlists", "reserved_union.txt")),
            "reserved_zig": sha256_file(
                os.path.join(HERE, "wordlists", "reserved_zig.txt")),
        },
        "lexer_guards": {
            "G1": "a WORD immediately preceded by OP '@' is a compiler builtin name and "
                  "is never substituted in either direction",
            "G2": "@\"...\" is a quoted identifier; its body lexes as STRING and is "
                  "therefore unreachable by substitution",
        },
        "not_transformed_in_I1": {
            "note": "methodology 10 section 7.1: V-roles keep their real spelling in I1; "
                    "ordinary user identifiers are never renamed.",
            "v_roles": V_ROLES_NOT_TRANSFORMED,
            "user_identifiers": ["std", "tio", "io", "wbuf", "out", "state", "sum",
                                 "largest", "evens", "first", "i", "term"],
        },
        "mapping": mapping,
        "reverse": reverse,
        "collision_redraws": redraws,
        "rejection_filters_fired": {k: v for k, v in FILTERS_FIRED.items() if v},
        "anonymized_token_count": len(mapping),
    }
    return doc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "mapping.json"))
    ap.add_argument("--print", dest="do_print", action="store_true")
    ap.add_argument("--fixed-time", default=None)
    args = ap.parse_args()

    doc = build(args.fixed_time)
    text = json.dumps(doc, indent=2) + "\n"
    if args.do_print:
        sys.stdout.write(text)
    else:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print("wrote %s (%d tokens)" % (args.out, doc["anonymized_token_count"]))


if __name__ == "__main__":
    main()
