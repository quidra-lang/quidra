"""I1 round-trip / submission validator for language_id = python.

Modes:
  python3 validate.py roundtrip FILE.py        real -> forward -> inverse, R1/R4/R5
  python3 validate.py submission ANON.py EXPECTED.txt
                                               gate -> inverse -> compile -> run -> compare
  python3 validate.py selftest                 positive AND negative fixtures, with evidence

A validator that cannot fail measures nothing (methodology 10, 10.4 requirement 3),
so `selftest` exercises both directions and exits non-zero unless every positive
case is ACCEPTED and every negative case is REJECTED for the stated reason.

Note on the 10.4 warning: the "did any transformed token survive?" test is vacuous
when the mapping is a permutation of the language's own vocabulary. It is NOT
vacuous here -- I1 maps the real keywords onto pseudo-words that are provably not
tokens of this language (mapping.json rejection filters) -- but it is still weak on
its own, so the checks below also require the inverse image to COMPILE, RUN, and
produce the exact expected bytes. Negative case N5 is a submission that passes every
token-level check and is still rejected, on output.
"""

import json
import keyword
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import forward as fwd_mod      # noqa: E402
import inverse as inv_mod      # noqa: E402

PY = sys.executable or "python3"


def _mapping_doc():
    with open(os.path.join(HERE, "mapping.json")) as f:
        return json.load(f)


DOC = _mapping_doc()
FWD = {real: e["pseudo_word"] for real, e in DOC["mapping"].items()}
INV = {e["pseudo_word"]: real for real, e in DOC["mapping"].items()}

# The real surface a conforming I1 submission must never spell: this language's
# reserved words, plus every real token the mapping replaced (some of which --
# the type names -- are builtins rather than reserved words).
REAL_SURFACE = set(keyword.kwlist) | set(FWD)


# ------------------------------------------------------------------ primitives

def gate(anon_text):
    """I1 conformance gate (methodology 10, 4.2a). Returns list of H_REAL events."""
    hits = []
    for kind, text in inv_mod.tokenize(anon_text):
        if kind == fwd_mod.WORD and text in REAL_SURFACE:
            hits.append(text)
    return hits


def build_and_run(real_text, timeout=10):
    """Compile and run real source. Returns (ok, stage, stdout, stderr, rc)."""
    d = tempfile.mkdtemp(prefix="i1py_")
    path = os.path.join(d, "solution.py")
    with open(path, "w") as f:
        f.write(real_text)
    c = subprocess.run([PY, "-m", "py_compile", path],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if c.returncode != 0:
        return False, "COMPILE", "", c.stderr.decode(), c.returncode
    try:
        r = subprocess.run([PY, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT", "", "", -1
    if r.returncode != 0:
        return False, "RUN", r.stdout.decode(), r.stderr.decode(), r.returncode
    return True, "OK", r.stdout.decode(), r.stderr.decode(), 0


def word_multiset(text):
    return [t for k, t in inv_mod.tokenize(text) if k == fwd_mod.WORD]


# ------------------------------------------------------------------ checks

def check_roundtrip(real_text):
    """R1 byte identity, R4 non-triviality, R5 domain containment."""
    reasons = []
    try:
        anon = fwd_mod.forward_text(real_text, FWD)
    except ValueError as e:
        return False, [str(e)], None
    back = inv_mod.inverse_text(anon, INV)
    if back != real_text:
        reasons.append("R1_BYTE_IDENTITY_FAILED")
    if anon == real_text:
        reasons.append("R4_TRANSFORM_WAS_A_NO_OP")
    a, b = word_multiset(real_text), word_multiset(anon)
    if len(a) != len(b):
        reasons.append("R5_TOKEN_COUNT_CHANGED")
    else:
        for x, y in zip(a, b):
            if x != y and not (x in FWD and FWD[x] == y):
                reasons.append("R5_OUT_OF_DOMAIN_CHANGE:%s->%s" % (x, y))
    return (not reasons), reasons, anon


def check_submission(anon_text, expected_stdout):
    """Full pipeline: gate -> inverse -> compile -> run -> byte-compare stdout."""
    reasons = []
    hits = gate(anon_text)
    if hits:
        reasons.append("GATE_H_REAL:" + ",".join(sorted(set(hits))))
        return False, reasons, ""
    real = inv_mod.inverse_text(anon_text, INV)
    ok, stage, out, err, rc = build_and_run(real)
    if not ok:
        reasons.append("%s_FAILED:rc=%s:%s" % (stage, rc, err.strip().splitlines()[-1:]))
        return False, reasons, out
    if out != expected_stdout:
        reasons.append("OUTPUT_MISMATCH")
        return False, reasons, out
    return True, [], out


# ------------------------------------------------------------------ selftest

def _read(p):
    with open(os.path.join(HERE, p)) as f:
        return f.read()


def selftest():
    failures = []
    print("=" * 72)
    print("I1 python validator self-test -- seed %d" % DOC["seed"])
    print("=" * 72)

    real = _read("fixture_real.py")
    anon = _read("fixture_anon.py")
    ok, stage, fixture_out, err, rc = build_and_run(real)
    if not ok:
        failures.append("fixture_real.py does not build/run")
    print("\nfixture_real.py runs, exit 0, stdout:")
    print("".join("    | " + l + "\n" for l in fixture_out.splitlines()))

    def case(name, expect, fn):
        got_ok, reasons = fn()
        verdict = "ACCEPTED" if got_ok else "REJECTED"
        want = "ACCEPTED" if expect else "REJECTED"
        mark = "PASS" if verdict == want else "FAIL"
        if mark == "FAIL":
            failures.append(name)
        print("  [%s] %-28s expected %-8s got %-8s %s"
              % (mark, name, want, verdict, ("reasons=" + ";".join(reasons)) if reasons else ""))

    print("\n-- positive cases (must be ACCEPTED) --")
    case("P1 roundtrip fixture", True,
         lambda: check_roundtrip(real)[:2])
    case("P2 roundtrip reference sol", True,
         lambda: check_roundtrip(_read("reference_solution.py"))[:2])
    case("P3 submission fixture_anon", True,
         lambda: check_submission(anon, fixture_out)[:2])
    case("P4 anon == forward(real)", True,
         lambda: (anon == fwd_mod.forward_text(real, FWD), []))

    print("\n-- negative cases (must be REJECTED) --")
    # N1: one pseudo-word replaced by the real keyword it stands for.
    n1 = anon.replace("muveze ", "if ", 1)
    case("N1 real keyword reinstated", False,
         lambda: check_submission(n1, fixture_out)[:2])
    # N2: correct real source that ignores the transformation entirely (PF-05 b).
    case("N2 untransformed real source", False,
         lambda: check_submission(real, fixture_out)[:2])
    # N3: a pseudo-word corrupted into an undefined name.
    n3 = anon.replace("lodira", "lodiro")
    case("N3 corrupted pseudo-word", False,
         lambda: check_submission(n3, fixture_out)[:2])
    # N4: source in which a pseudo-word is used as a user identifier (injectivity).
    n4 = real.replace("value", "lodira")
    case("N4 pseudo-word as identifier", False,
         lambda: check_roundtrip(n4)[:2])
    # N5: passes every token-level check, still wrong -- arithmetic altered.
    n5 = anon.replace("odds = odds + 1", "odds = odds + 2")
    case("N5 token-clean, wrong output", False,
         lambda: check_submission(n5, fixture_out)[:2])
    # N6: round-trip broken by a transformer that also rewrote a string literal.
    n6_anon = fwd_mod.forward_text(real, FWD).replace('"TOTAL ', '"digami ')
    case("N6 literal touched", False,
         lambda: (inv_mod.inverse_text(n6_anon, INV) == real,
                  ["R1_BYTE_IDENTITY_FAILED"]))

    print("\n-- rejection-filter liveness: the real accept() of gen_mapping.py --")
    import gen_mapping
    accept, _fired = gen_mapping.make_accept()
    used = set(INV)
    probes = (
        ("filter1 shape", "ab3de", False, "not ^[a-z]{6}$"),
        ("filter2 collision", sorted(INV)[0], False, "already used"),
        ("filter3 english", "banana", False, "in en_common.txt"),
        ("filter4 progterm", "buffer", False, "in prog_terms.txt"),
        ("filter5 reserved", "static", False, "in reserved_union.txt"),
        ("filter6 substring", "zolist", False, "contains 'list'"),
        ("filter7 material", "biggest", False, "occurs in the fixture material"),
        ("control accepted", "gidopu", True, "trips no filter"),
    )
    for label, cand, want, why in probes:
        got = accept(cand, used)
        mark = "PASS" if got == want else "FAIL"
        if mark == "FAIL":
            failures.append(label)
        print("    [%s] %-18s %-8s accept()=%-5s expected %-5s (%s)"
              % (mark, label, cand, got, want, why))

    print("\n" + "=" * 72)
    if failures:
        print("SELFTEST FAILED: " + ", ".join(failures))
        return 1
    print("SELFTEST PASSED: every positive accepted, every negative rejected.")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) == 3 and argv[1] == "roundtrip":
        ok, reasons, _ = check_roundtrip(open(argv[2]).read())
        print("ACCEPTED" if ok else "REJECTED " + ";".join(reasons))
        return 0 if ok else 1
    if len(argv) == 4 and argv[1] == "submission":
        ok, reasons, _ = check_submission(open(argv[2]).read(), open(argv[3]).read())
        print("ACCEPTED" if ok else "REJECTED " + ";".join(reasons))
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
