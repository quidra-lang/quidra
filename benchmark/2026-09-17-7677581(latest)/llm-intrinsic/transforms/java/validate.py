#!/usr/bin/env python3
"""Round-trip validator and conformance gate for I1 / java / seed 20260918.

Two jobs, both of which must demonstrably be able to FAIL:

  round_trip(real_source)            -> R1..R5 of methodology 10 section 6.4
  gate(transformed_submission)       -> the section 4.2a conformance gate for I1

Run with no arguments to execute the full positive+negative battery used as the
evidence for preflight.md check (c).

  validate.py                 # full battery, exit 0 iff every case behaves as required
  validate.py --gate FILE     # gate one file, exit 0 = conformant
  validate.py --rt FILE       # round-trip one real source, exit 0 = R1..R5 pass
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from javalex import tokenize  # noqa: E402
from forward import forward, load_mapping  # noqa: E402
from inverse import inverse, load_inverse_mapping, ambiguity_report  # noqa: E402

JAVA_HOME = "/opt/homebrew/opt/openjdk"
JAVAC = os.path.join(JAVA_HOME, "bin", "javac")
JAVA = os.path.join(JAVA_HOME, "bin", "java")

RESERVED = [
    ln.strip()
    for ln in open(os.path.join(HERE, "wordlists", "reserved_java.txt"), encoding="ascii")
    if ln.strip()
]
RESERVED_SET = set(RESERVED)

# Names the pack documents in their real, untransformed spelling (I1 does not
# transform standard-vocabulary names).  None of them is a reserved word, so the
# gate's allow-list is empty in practice; it is written out so the rule is explicit.
PACK_UNTRANSFORMED = set()


# --------------------------------------------------------------------------- #
# the conformance gate (section 4.2a, condition I1)
# --------------------------------------------------------------------------- #
def gate(transformed_source):
    """Reject a submission that ignored the transformation and wrote real keywords.

    Only WORD tokens are examined; a reserved word occurring inside a string
    literal, a character literal or a comment is not a violation.
    """
    events = []
    for kind, raw in tokenize(transformed_source):
        if kind != "WORD":
            continue
        if raw in RESERVED_SET and raw not in PACK_UNTRANSFORMED:
            events.append(raw)
    return (len(events) == 0), events


# --------------------------------------------------------------------------- #
# build / run helpers
# --------------------------------------------------------------------------- #
def build_and_run(real_source):
    """Return (build_ok, run_ok, stdout, diagnostics)."""
    d = tempfile.mkdtemp(prefix="i1java-")
    try:
        src = os.path.join(d, "Main.java")
        with open(src, "w", encoding="utf-8") as f:
            f.write(real_source)
        out = os.path.join(d, "out")
        p = subprocess.run([JAVAC, "-J-Duser.language=en", "-J-Duser.country=US",
                            "-d", out, src], capture_output=True, text=True)
        if p.returncode != 0:
            return False, False, "", p.stderr.strip()
        r = subprocess.run([JAVA, "-cp", out, "Main"], capture_output=True, text=True, timeout=10)
        return True, r.returncode == 0, r.stdout, r.stderr.strip()
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --------------------------------------------------------------------------- #
# the round-trip validator (section 6.4, R1..R5)
# --------------------------------------------------------------------------- #
def round_trip(real_source, expected_stdout=None):
    mapping = load_mapping()
    inv = load_inverse_mapping()
    res = {}

    t = forward(real_source, mapping)
    back = inverse(t, inv)

    res["R1_byte_identity"] = (back == real_source)
    res["R4_non_triviality"] = (t != real_source and forward.last_changed > 0)

    # R5 domain containment: every token that differs between F and forward(F)
    # must be a bound token replaced by its own pseudo-word, position for position.
    a = tokenize(real_source)
    b = tokenize(t)
    ok5 = len(a) == len(b)
    if ok5:
        for (ka, ra), (kb, rb) in zip(a, b):
            if ra == rb:
                continue
            if not (ka == "WORD" and kb == "WORD" and mapping.get(ra) == rb):
                ok5 = False
                break
    res["R5_domain_containment"] = ok5

    built, ran, stdout, diag = build_and_run(back)
    res["R2_build_identity"] = built and ran
    if expected_stdout is None:
        _, _, ref_stdout, _ = build_and_run(real_source)
        expected_stdout = ref_stdout
    res["R3_behavioral_identity"] = (stdout == expected_stdout)
    res["_stdout"] = stdout
    res["_diagnostics"] = diag
    res["_ok"] = all(v for k, v in res.items() if not k.startswith("_"))
    return res


# --------------------------------------------------------------------------- #
# the filter self-test: show each accept() rejection rule can fire
# --------------------------------------------------------------------------- #
def filter_self_test():
    import lexicalize

    accept, fired, _ = lexicalize.build_accept()
    cases = [
        ("1 shape", "toolong", set()),          # not ^[a-z]{6}$
        ("2 used", "mepone", {"mepone"}),       # already taken
        ("3 en_common", "animal", set()),       # a natural-language word
        ("4 prog_terms", "buffer", set()),      # a common programming term
        ("5 reserved_union", "typeof", set()),  # a reserved word of one of the ten languages
        ("6 substring", "zitemu", set()),       # contains the >=4-letter term "item"
        ("7 material", "rintln", set()),        # occurs inside the task material
    ]
    results = []
    for label, word, used in cases:
        before = dict(fired)
        ok = accept(word, used)
        delta = [k for k in fired if fired[k] != before[k]]
        results.append((label, word, ok, delta))
    return results


# --------------------------------------------------------------------------- #
# battery
# --------------------------------------------------------------------------- #
def read(p):
    return open(os.path.join(HERE, p), encoding="utf-8").read()


def battery():
    failures = []
    print("=" * 72)
    print("POSITIVE CASES (must pass)")
    print("=" * 72)

    real = read("fixture_real.java")
    expected = read("expected_output.txt")

    r = round_trip(real, expected)
    for k in ("R1_byte_identity", "R2_build_identity", "R3_behavioral_identity",
              "R4_non_triviality", "R5_domain_containment"):
        print("  P1 round_trip(fixture_real.java)  %-24s %s" % (k, "PASS" if r[k] else "FAIL"))
        if not r[k]:
            failures.append("P1/" + k)

    anon = read("fixture_anon.java")
    ok, ev = gate(anon)
    print("  P2 gate(fixture_anon.java)            conformant=%s events=%s  %s"
          % (ok, ev, "PASS" if ok else "FAIL"))
    if not ok:
        failures.append("P2")

    # a pseudo-word or a real keyword inside a string literal is NOT a violation
    lit = read("neg/lit_in_string.java")
    ok, ev = gate(forward(lit))
    print("  P3 gate(source whose STRING literal contains real keywords) "
          "conformant=%s  %s" % (ok, "PASS" if ok else "FAIL"))
    if not ok:
        failures.append("P3")
    t_lit = forward(lit)
    kept = '"if else class static"' in t_lit
    print("  P4 forward() left the string literal untouched: %s  %s"
          % (kept, "PASS" if kept else "FAIL"))
    if not kept:
        failures.append("P4")

    print()
    print("=" * 72)
    print("NEGATIVE CASES (must be REJECTED)")
    print("=" * 72)

    # N1 -- correct real source that ignores the transformation entirely.
    ok, ev = gate(real)
    rejected = not ok
    print("  N1 gate(correct REAL source, transformation ignored)")
    print("     conformant=%s  H_REAL events=%s" % (ok, sorted(set(ev))))
    print("     -> %s" % ("REJECTED (PASS)" if rejected else "ACCEPTED (FAIL)"))
    if not rejected:
        failures.append("N1")

    # N2 -- transformed source with one pseudo-word corrupted by one letter.
    corrupt = read("neg/corrupt_pseudoword.java")
    b = inverse(corrupt)
    rt_ok = (b == real)
    built, ran, out, diag = build_and_run(b)
    rejected = (not rt_ok) and (not (built and ran))
    print("  N2 round_trip on a CORRUPTED transformed source (one letter changed)")
    print("     R1 byte identity vs real source: %s" % rt_ok)
    print("     inverse image builds: %s   diagnostic: %s"
          % (built, (diag.splitlines() or [""])[0][:100]))
    print("     -> %s" % ("REJECTED (PASS)" if rejected else "ACCEPTED (FAIL)"))
    if not rejected:
        failures.append("N2")

    # N3 -- transformed source in which one pseudo-word was written as the real keyword.
    leak = read("neg/keyword_leak.java")
    ok, ev = gate(leak)
    rejected = not ok
    print("  N3 gate(transformed source with ONE real keyword written from memory)")
    print("     conformant=%s  H_REAL events=%s" % (ok, sorted(set(ev))))
    print("     -> %s" % ("REJECTED (PASS)" if rejected else "ACCEPTED (FAIL)"))
    if not rejected:
        failures.append("N3")

    # N4 -- submission that names one of its own variables with a pseudo-word.
    amb = read("neg/inverse_ambiguous.java")
    hits = ambiguity_report(amb)
    b = inverse(amb)
    built, ran, out, diag = build_and_run(b)
    rejected = (len(hits) > 0) and (not built)
    print("  N4 INVERSE_AMBIGUOUS: a variable named with a pseudo-word")
    print("     detector hits=%d  inverse image builds=%s" % (len(hits), built))
    print("     diagnostic: %s" % (diag.splitlines() or [""])[0][:100])
    print("     -> %s" % ("REJECTED (PASS)" if rejected else "ACCEPTED (FAIL)"))
    if not rejected:
        failures.append("N4")

    # N5 -- a mutated round trip: drop the mapping for one token and re-run R5.
    mutated = dict(load_mapping())
    mutated.pop("if")
    t = forward(real, mutated)
    ok5 = "if" not in [raw for kind, raw in tokenize(t) if kind == "WORD"]
    rejected = not ok5
    print("  N5 round_trip with a MUTATED mapping (one token left unmapped)")
    print("     transformed source still contains the real token 'if': %s" % (not ok5))
    print("     -> %s" % ("REJECTED (PASS)" if rejected else "ACCEPTED (FAIL)"))
    if not rejected:
        failures.append("N5")

    print()
    print("=" * 72)
    print("LEXICALIZER FILTER SELF-TEST (each accept() rule must be able to fire)")
    print("=" * 72)
    for label, word, ok, delta in filter_self_test():
        good = (not ok)
        print("  filter %-18s candidate=%-8s accepted=%-5s fired=%s  %s"
              % (label, word, ok, delta, "PASS" if good else "FAIL"))
        if not good:
            failures.append("FILTER/" + label)

    print()
    print("=" * 72)
    if failures:
        print("BATTERY FAILED:", failures)
        return 1
    print("BATTERY PASSED: every positive case passed and every negative case was rejected.")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "--gate":
        ok, ev = gate(open(argv[2], encoding="utf-8").read())
        print(json.dumps({"conformant": ok, "events": sorted(set(ev))}))
        return 0 if ok else 1
    if len(argv) >= 3 and argv[1] == "--rt":
        r = round_trip(open(argv[2], encoding="utf-8").read())
        print(json.dumps({k: v for k, v in r.items() if not k.startswith("_")}, indent=2))
        return 0 if r["_ok"] else 1
    return battery()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
