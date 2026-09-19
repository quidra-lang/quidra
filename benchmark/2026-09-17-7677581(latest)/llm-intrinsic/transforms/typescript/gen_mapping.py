"""Generate mapping.json for I1 (Keyword Anonymization), language_id = typescript.

Implements methodology 10, section 5 (controlled random lexicalization):
  5.1 deterministic primitives (FNV1a32, seed_state, Park-Miller next, pick)
  5.2 pseudo-word generation PW-1 (CVCVCV, exactly six ASCII lowercase chars)
  5.3 assignment procedure with the frozen accept() rejection filters

Deterministic: same seed -> same mapping, in any process, on any host.
Run:  python3 gen_mapping.py
"""

import hashlib
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

CONDITION_ID = "I1"
LANGUAGE_ID = "typescript"
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


# ---------------------------------------------------------------- role table

# role_token_key -> (real token, language-neutral role prose)
# Domain rule (methodology 10, 7.1 / 7.1a): every BOUND K-role token this
# language needs for the frozen task, plus every non-role reserved word that
# occurs in the fixtures or in a conforming solution.  For this language the
# two sets coincide: the fixture and the reference solution use no reserved
# word outside the K-role bindings below.
# V-roles (console.log, push, join, String, length) are NOT transformed in I1.
ROLES = {
    "K01": ("function", "introduces a function definition"),
    "K02": ("const", "introduces a named value binding that may not be re-bound afterwards"),
    "K03": ("let", "introduces a named value binding that may be re-bound afterwards"),
    "K04": ("if", "introduces a conditional branch"),
    "K05": ("else", "introduces the alternative branch"),
    "K07#1": ("for", "introduces bounded iteration"),
    "K07#2": ("of", "separates the iteration name from the sequence it walks"),
    "K08": ("while", "introduces conditional iteration"),
    "K09": ("break", "exits the innermost loop"),
    "K10": ("continue", "proceeds to the next iteration of the innermost loop"),
    "K11": ("return", "yields a function result"),
    "K16": ("true", "the boolean true literal"),
    "K17": ("false", "the boolean false literal"),
    "K21a": ("number", "the numeric type name used by the task; whole-number values are exact"),
    "K21b": ("boolean", "the boolean type name"),
    "K21c": ("string", "the text type name"),
    "K21e": ("void", "the result-less designation: the result type of a function that yields nothing"),
}

NOT_LEXICALIZED = {
    "K06": "expressed as the K05 token followed by the K04 token; this language has no distinct chained-alternative word",
    "K12": "no aggregate type is needed by the task set",
    "K13": "no aggregate type is needed by the task set",
    "K14": "the task needs no import; every facility it uses is globally available",
    "K15": "this language needs no compilation-unit declaration for a single-file program",
    "K18": "logical conjunction is punctuation in this language, not a word token (NOT_WORD_TOKEN)",
    "K19": "logical disjunction is punctuation in this language, not a word token (NOT_WORD_TOKEN)",
    "K20": "logical negation is punctuation in this language, not a word token (NOT_WORD_TOKEN)",
    "K21d": "the dynamic-sequence type is written as a punctuation suffix on the element type, not as a word token (NOT_WORD_TOKEN)",
    "K22": "this language marks no entry point with a word token; top-level statements run in order",
    "K23": "this language needs no visibility or linkage marker for a single-file program",
    "K24": "this language has no mutability qualifier distinct from the K03 binding word",
    "K25": "the type annotation is introduced by punctuation, not by a word token (NOT_WORD_TOKEN)",
    "K26": "no conversion word token is needed; conversion is a named facility, which I1 does not transform",
    "K27": "the output facility of this language declares no error propagation",
}

# ---------------------------------------------------------------- accept()

MATERIAL_FILES = (
    "fixture_real.ts",
    "fixture_expected_output.txt",
    "reference_solution.ts",
    "expected_output.txt",
    "task_statement.txt",
)


def load_list(name):
    with open(os.path.join(HERE, "wordlists", name), "r") as f:
        return set(x.strip() for x in f if x.strip())


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def material_text():
    """Filter 7: a pseudo-word may not occur anywhere in the task material."""
    chunks = []
    for name in MATERIAL_FILES:
        with open(os.path.join(HERE, name), "r") as f:
            chunks.append(f.read())
    return "\n".join(chunks)


def make_accept():
    """Return (accept, filters_fired). accept(w, used) -> bool, per section 5.3."""
    en_common = load_list("en_common.txt")
    prog_terms = load_list("prog_terms.txt")
    reserved_union = load_list("reserved_union.txt")
    reserved_ts = load_list("reserved_typescript.txt")
    substr_ban = sorted(
        w for w in (en_common | prog_terms | reserved_union | reserved_ts) if len(w) >= 4)
    material = material_text()

    filters_fired = {str(i): 0 for i in range(1, 8)}

    def accept(w, used):
        ok = True
        if not (len(w) == 6 and all("a" <= c <= "z" for c in w)):
            filters_fired["1"] += 1
            ok = False
        if w in used:
            filters_fired["2"] += 1
            ok = False
        if w in en_common:
            filters_fired["3"] += 1
            ok = False
        if w in prog_terms:
            filters_fired["4"] += 1
            ok = False
        if w in reserved_union or w in reserved_ts:
            filters_fired["5"] += 1
            ok = False
        for b in substr_ban:
            if b in w:
                filters_fired["6"] += 1
                ok = False
                break
        if w in material:
            filters_fired["7"] += 1
            ok = False
        return ok

    return accept, filters_fired


def build_mapping():
    accept, filters_fired = make_accept()
    assigned = {}
    used = set()
    redraws = {}
    for rk in sorted(ROLES):
        key = "%s|%s|%d|%s" % (CONDITION_ID, LANGUAGE_ID, SEED, rk)
        st = seed_state(key)
        got = None
        for attempt in range(1, 10001):
            w, st = draw_word(st)
            if accept(w, used):
                got = w
                if attempt > 1:
                    redraws[rk] = attempt - 1
                break
        if got is None:
            raise SystemExit("LEXICALIZATION_EXHAUSTION " + key)
        assigned[rk] = got
        used.add(got)
    return assigned, redraws, filters_fired


def main():
    assigned, redraws, filters_fired = build_mapping()
    en_common = load_list("en_common.txt")
    prog_terms = load_list("prog_terms.txt")
    reserved_union = load_list("reserved_union.txt")
    reserved_ts = load_list("reserved_typescript.txt")

    mapping = {}
    for rk in sorted(ROLES):
        real, prose = ROLES[rk]
        mapping[real] = {
            "role_token_key": rk,
            "pseudo_word": assigned[rk],
            "role_prose": prose,
        }

    doc = {
        "unit": "I1/s%d" % SEED,
        "condition": CONDITION_ID,
        "language_id": LANGUAGE_ID,
        "seed": SEED,
        "primitives": {
            "rng": "park-miller-48271",
            "hash": "fnv1a32",
            "pick_divisor": 128,
            "pseudo_word_shape": "CVCVCV, exactly 6 ASCII lowercase characters",
            "key_format": "<condition_id>|<language_id>|<decimal seed>|<role_token_key>",
        },
        "wordlist_sha256": {
            "en_common": sha256_file(os.path.join(HERE, "wordlists", "en_common.txt")),
            "prog_terms": sha256_file(os.path.join(HERE, "wordlists", "prog_terms.txt")),
            "reserved_union": sha256_file(os.path.join(HERE, "wordlists", "reserved_union.txt")),
            "reserved_typescript": sha256_file(
                os.path.join(HERE, "wordlists", "reserved_typescript.txt")),
        },
        "wordlist_sizes": {
            "en_common": len(en_common),
            "prog_terms": len(prog_terms),
            "reserved_union": len(reserved_union),
            "reserved_typescript": len(reserved_ts),
        },
        "anonymized_token_count": len(mapping),
        "mapping": mapping,
        "not_lexicalized": NOT_LEXICALIZED,
        "not_transformed_in_I1": {
            "reason": "V-roles keep their real spellings in I1 (methodology 10, 7.1)",
            "tokens": ["console.log", "push", "join", "String", "length"],
        },
        "collision_redraws": redraws,
        "rejection_filters_fired": filters_fired,
        "pseudo_word_char_counts": {w: 6 for w in sorted(assigned.values())},
    }

    out = os.path.join(HERE, "mapping.json")
    with open(out, "w") as f:
        json.dump(doc, f, indent=2, sort_keys=False)
        f.write("\n")
    print("wrote", out, "tokens=", len(mapping))
    for real in sorted(mapping):
        print("  %-9s -> %s" % (real, mapping[real]["pseudo_word"]))


if __name__ == "__main__":
    main()
