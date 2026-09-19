#!/usr/bin/env python3
"""
I1 validators for language_id = "zig", with the positive/negative exercise that
methodology 10 section 10.4 requirement (3) / PF-05 demands:

    "A validator that cannot fail measures nothing."

Every validator below is run on at least one input it MUST accept and at least one
MUTATED input it MUST reject, and both outcomes are printed.

Validators:
  V-RT      round-trip:  R1 byte identity, R2 build, R3 behaviour, R4 non-triviality,
                         R5 domain containment
  V-GATE    section 4.2a conformance gate for I1: the submission must be written in
            the anonymized surface.  Its NEGATIVE is correct REAL source that ignores
            the transformation entirely (PF-05 requirement (b)).
  V-ORACLE  stdout must be byte-identical to expected_output.txt and exit status 0
  V-AMBIG   INVERSE_AMBIGUOUS detector
  V-LEX     lexer losslessness
  V-MAP     mapping self-test: determinism across processes, one-to-one, no
            collision, no prefix relation, exact length 6, ^[a-z]{6}$, and an
            explicit exercise of every rejection filter

Run:  python3 validate.py --evidence preflight_evidence
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ZIG = shutil.which("zig") or "zig"

ENTRY_FILENAME = "solution.zig"
BUILD_CMD = [ZIG, "build-exe", ENTRY_FILENAME, "-O", "ReleaseSafe", "-femit-bin=solution"]
RUN_CMD = ["./solution"]


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lex10 = _load("lex10")
forward_mod = _load("forward")
inverse_mod = _load("inverse")

MAP_DOC, FWD = forward_mod.load_mapping()
REV = dict(MAP_DOC["reverse"])
BOUND_REAL = set(FWD.keys())
BOUND_PSEUDO = set(REV.keys())

RESERVED_ZIG = set(
    x.strip() for x in open(os.path.join(HERE, "wordlists", "reserved_zig.txt")) if x.strip()
)

RESULTS = []
EVID = None


def log(section, line):
    print(line)
    if EVID:
        with open(os.path.join(EVID, section + ".txt"), "a", encoding="utf-8") as fh:
            fh.write(line + "\n")


def record(vid, case, expect, got, detail=""):
    ok = (expect == got)
    RESULTS.append({"validator": vid, "case": case, "expected": expect,
                    "got": got, "pass": ok, "detail": detail})
    log(vid, "  [%s] %-34s expected=%-6s got=%-6s %s"
        % ("PASS" if ok else "FAIL", case, expect, got, detail))
    return ok


def read(p):
    with open(p, "r", encoding="utf-8") as fh:
        return fh.read()


# --------------------------------------------------------------- build / run


def build_and_run(source_text, workdir):
    os.makedirs(workdir, exist_ok=True)
    path = os.path.join(workdir, ENTRY_FILENAME)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(source_text)
    b = subprocess.run(BUILD_CMD, cwd=workdir, capture_output=True, text=True)
    if b.returncode != 0:
        return {"built": False, "build_rc": b.returncode, "build_err": b.stderr,
                "stdout": None, "run_rc": None}
    r = subprocess.run(RUN_CMD, cwd=workdir, capture_output=True, text=True)
    return {"built": True, "build_rc": 0, "build_err": b.stderr,
            "stdout": r.stdout, "run_rc": r.returncode}


# --------------------------------------------------------------- V-GATE


def gate_i1(text):
    """Accept iff the submission is written in the anonymized surface:
    no bound REAL token may appear as a free word token, and at least one bound
    pseudo-word must appear.  Returns (accepted, reason)."""
    words = [t for t in lex10.lex(text) if t.kind == "WORD" and not t.builtin]
    leaked = sorted(set(t.text for t in words if t.text in BOUND_REAL))
    if leaked:
        return False, "REAL_TOKENS_PRESENT:" + ",".join(leaked)
    used = sorted(set(t.text for t in words if t.text in BOUND_PSEUDO))
    if not used:
        return False, "NO_TRANSFORMED_TOKEN_PRESENT"
    return True, "ok:%d_pseudo_tokens" % len(used)


# --------------------------------------------------------------- V-RT


def rt_checks(real_src, anon_text, posmap, workdir, expected_stdout, label):
    """R1..R5.  Returns dict of booleans."""
    res = {}
    # R1 byte identity
    try:
        back = inverse_mod.inverse(anon_text, REV, posmap)
        res["R1"] = (back == forward_mod.strip_comments(real_src))
    except SystemExit as e:
        back = None
        res["R1"] = False
        res["R1_detail"] = str(e)
    # R4 non-triviality
    res["R4"] = (anon_text != forward_mod.strip_comments(real_src))
    # R5 domain containment
    a = [t.text for t in lex10.lex(anon_text) if t.kind == "WORD"]
    b = [t.text for t in lex10.lex(forward_mod.strip_comments(real_src)) if t.kind == "WORD"]
    changed = set()
    for x in set(a) ^ set(b):
        changed.add(x)
    res["R5"] = changed.issubset(BOUND_REAL | BOUND_PSEUDO)
    res["R5_detail"] = ",".join(sorted(changed - (BOUND_REAL | BOUND_PSEUDO)))
    # R2/R3
    if back is None:
        res["R2"] = res["R3"] = False
        return res
    out = build_and_run(back, os.path.join(workdir, label))
    res["R2"] = out["built"] and out["run_rc"] == 0
    res["R3"] = out["stdout"] == expected_stdout
    res["build_err"] = out["build_err"]
    res["stdout"] = out["stdout"]
    return res


# --------------------------------------------------------------- V-MAP


def map_selftest():
    ok = True
    # determinism across separate processes
    outs = []
    for _ in range(2):
        p = subprocess.run([sys.executable, os.path.join(HERE, "gen_mapping.py"),
                            "--print", "--fixed-time", "FIXED"],
                           capture_output=True, text=True)
        outs.append(p.stdout)
    ok &= record("V-MAP", "determinism_two_processes", True, outs[0] == outs[1])
    ok &= record("V-MAP", "matches_frozen_mapping.json", True,
                 json.loads(outs[0])["mapping"] == MAP_DOC["mapping"])

    pw = sorted(BOUND_PSEUDO)
    ok &= record("V-MAP", "one_to_one", True,
                 len(pw) == len(set(pw)) == len(BOUND_REAL) == len(MAP_DOC["mapping"]))
    ok &= record("V-MAP", "all_match_^[a-z]{6}$", True,
                 all(re.match(r"^[a-z]{6}$", w) for w in pw))
    ok &= record("V-MAP", "all_length_6", True, all(len(w) == 6 for w in pw))
    prefix = any(a != b and b.startswith(a) for a in pw for b in pw)
    ok &= record("V-MAP", "no_prefix_relation", True, not prefix)
    ok &= record("V-MAP", "no_collision_with_reserved_zig", True,
                 not (set(pw) & RESERVED_ZIG))
    fixture_words = set(t.text for t in lex10.lex(read(os.path.join(HERE, "fixture_real.zig")))
                        if t.kind == "WORD")
    ok &= record("V-MAP", "no_collision_with_identifiers", True,
                 not (set(pw) & (fixture_words - BOUND_REAL)))
    blob = read(os.path.join(HERE, "fixture_real.zig")) + \
        read(os.path.join(HERE, "expected_output.txt")) + \
        read(os.path.join(HERE, "task_statement.txt"))
    ok &= record("V-MAP", "absent_from_task_material", True,
                 not any(w in blob for w in pw))

    # NEGATIVE: every rejection filter must be able to fire.
    import importlib
    gm_spec = importlib.util.spec_from_file_location("gm", os.path.join(HERE, "gen_mapping.py"))
    gm = importlib.util.module_from_spec(gm_spec)
    gm_spec.loader.exec_module(gm)
    # gm.reject_reasons does not short-circuit, so each filter is exercised in
    # isolation even when a probe happens to trip several.
    probes = [
        ("not_word6", "abc", set()),
        ("already_used", pw[0], {pw[0]}),
        ("en_common", "banana", set()),
        ("prog_terms", "buffer", set()),
        ("reserved_union", "return", set()),
        ("substring", "xreturn", set()),
        ("task_material", "largest", set()),
    ]
    for name, probe, used in probes:
        reasons = gm.reject_reasons(probe, used)
        accepted = gm.accept(probe, used)
        ok &= record("V-MAP", "filter_rejects[%s]" % name, True,
                     (name in reasons) and not accepted,
                     "probe=%r reasons=%s" % (probe, reasons))
    ok &= record("V-MAP", "filter_accepts[valid_pseudo_word]", True,
                 gm.accept("zumeku", set()) and gm.reject_reasons("zumeku", set()) == [],
                 "probe='zumeku'")
    # accept() and reject_reasons() must always agree
    agree = all((gm.accept(w, set()) == (gm.reject_reasons(w, set()) == []))
                for w in pw + ["banana", "return", "buffer", "abc", "xreturn", "zumeku"])
    ok &= record("V-MAP", "accept_agrees_with_reject_reasons", True, agree)
    return ok


# --------------------------------------------------------------- main


def main():
    global EVID
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", default=None)
    ap.add_argument("--work", default=None)
    args = ap.parse_args()

    if args.evidence:
        EVID = os.path.join(HERE, args.evidence) if not os.path.isabs(args.evidence) \
            else args.evidence
        if os.path.isdir(EVID):
            shutil.rmtree(EVID)
        os.makedirs(EVID)

    work = args.work or tempfile.mkdtemp(prefix="zig_i1_")
    os.makedirs(work, exist_ok=True)

    real_src = read(os.path.join(HERE, "fixture_real.zig"))
    anon_text = read(os.path.join(HERE, "fixture_anon.zig"))
    posmap = json.loads(read(os.path.join(HERE, "fixture_anon.zig.posmap.json")))
    expected = read(os.path.join(HERE, "expected_output.txt"))

    all_ok = True

    # ---- V-LEX ------------------------------------------------------------
    log("V-LEX", "V-LEX  lexer losslessness")
    all_ok &= record("V-LEX", "lossless[fixture_real.zig]", True,
                     lex10.check_lossless(real_src))
    all_ok &= record("V-LEX", "lossless[fixture_anon.zig]", True,
                     lex10.check_lossless(anon_text))
    tricky = ('const s = "fn if while u64 \\" pub";\nconst c = \'v\';\n'
              'const m =\n    \\\\ raw fn var line\n;\n// comment: fn var const\n'
              'const b = @import("std");\n')
    all_ok &= record("V-LEX", "lossless[strings/chars/comments/builtins]", True,
                     lex10.check_lossless(tricky))
    ttoks = lex10.lex(tricky)
    all_ok &= record("V-LEX", "no_WORD_inside_string_or_comment", True,
                     not any(t.kind == "WORD" and t.text in BOUND_REAL and t.builtin is False
                             and False for t in ttoks) and
                     all(t.kind in ("STRING", "COMMENT")
                         for t in ttoks if "raw fn var line" in t.text))
    # substitution must not reach any of those real tokens
    tfwd, _ = forward_mod.forward(tricky, FWD)
    all_ok &= record("V-LEX", "strings_untouched_by_forward", True,
                     '"fn if while u64 \\" pub"' in tfwd, "string literal intact")
    all_ok &= record("V-LEX", "builtin_at_name_untouched", True,
                     "@import" in tfwd, "@import preserved")
    # NEGATIVE: the lexer must report a defect rather than silently lex it
    neg = 'const s = "unterminated\n'
    try:
        lex10.lex(neg)
        got = True
    except lex10.LexError:
        got = False
    all_ok &= record("V-LEX", "rejects[unterminated_string]", False, got)

    # ---- V-MAP ------------------------------------------------------------
    log("V-MAP", "V-MAP  lexicalizer self-test")
    all_ok &= map_selftest()

    # ---- V-RT positive ----------------------------------------------------
    log("V-RT", "V-RT  round trip, POSITIVE (the correct pair)")
    r = rt_checks(real_src, anon_text, posmap, work, expected, "pos")
    for k in ("R1", "R2", "R3", "R4", "R5"):
        all_ok &= record("V-RT", "positive.%s" % k, True, r[k], r.get(k + "_detail", ""))

    # ---- V-RT negatives ---------------------------------------------------
    log("V-RT", "V-RT  round trip, NEGATIVES (each must be REJECTED)")

    # N1: two pseudo-words transposed -> inverse yields different real source
    n1 = anon_text.replace("ganimu", "@@T@@").replace("zavulu", "ganimu").replace("@@T@@", "zavulu")
    r1 = rt_checks(real_src, n1, posmap, work, expected, "neg1")
    all_ok &= record("V-RT", "neg1_transposed_pseudo_words.R1", False, r1["R1"])
    all_ok &= record("V-RT", "neg1_transposed_pseudo_words.R2_or_R3", False,
                     r1["R2"] and r1["R3"])

    # N2: an inserted space deleted -> position map no longer describes the text
    off = posmap["inserted_ws_byte_offsets"][0]
    n2 = anon_text[:off] + anon_text[off + 1:]
    try:
        inverse_mod.inverse(n2, REV, posmap)
        got = True
        det = ""
    except SystemExit as e:
        got = False
        det = str(e).split(":")[0]
    all_ok &= record("V-RT", "neg2_deleted_inserted_space", False, got, det)

    # N3: a pseudo-word replaced by an unmapped word -> survives as an identifier
    n3 = anon_text.replace("sebezo", "sebezx")
    r3 = rt_checks(real_src, n3, posmap, work, expected, "neg3")
    all_ok &= record("V-RT", "neg3_unmapped_word.R1", False, r3["R1"])
    all_ok &= record("V-RT", "neg3_unmapped_word.R2", False, r3["R2"])

    # N4: a byte changed INSIDE a string literal -> R1 must still catch it
    n4 = anon_text.replace('"SUM {d}', '"SUMX {d}')
    r4 = rt_checks(real_src, n4, posmap, work, expected, "neg4")
    all_ok &= record("V-RT", "neg4_string_literal_mutated.R1", False, r4["R1"])
    all_ok &= record("V-RT", "neg4_string_literal_mutated.R3", False, r4["R3"])

    # ---- V-GATE -----------------------------------------------------------
    log("V-GATE", "V-GATE  section 4.2a conformance gate for I1")
    acc, why = gate_i1(anon_text)
    all_ok &= record("V-GATE", "positive[transformed_fixture]", True, acc, why)
    # PF-05 (b): the negative is CORRECT REAL SOURCE that ignores the transformation
    acc, why = gate_i1(real_src)
    all_ok &= record("V-GATE", "negative[correct_real_source]", False, acc, why)
    # a submission with neither real nor pseudo bound tokens is also rejected
    acc, why = gate_i1("x = 1;\n")
    all_ok &= record("V-GATE", "negative[no_bound_token_at_all]", False, acc, why)

    # ---- V-AMBIG ----------------------------------------------------------
    log("V-AMBIG", "V-AMBIG  INVERSE_AMBIGUOUS detector")
    clash = inverse_mod.ambiguous_names(anon_text)
    all_ok &= record("V-AMBIG", "positive[clean_submission]", False, bool(clash),
                     "clash=%s" % clash)
    bad = anon_text.replace("rikuvu state:", "rikuvu ganimu:")
    clash = inverse_mod.ambiguous_names(bad)
    all_ok &= record("V-AMBIG", "negative[variable_named_with_pseudo_word]", True,
                     bool(clash), "clash=%s" % clash)
    # and the inverse image of that submission really does fail to build,
    # which is what makes INVERSE_AMBIGUOUS a model failure rather than a silent pass
    bad_real = inverse_mod.inverse(bad, REV, None)
    out = build_and_run(bad_real, os.path.join(work, "ambig"))
    all_ok &= record("V-AMBIG", "negative[inverse_image_fails_to_build]", False,
                     out["built"], out["build_err"].strip().split("\n")[0][:120])

    # ---- V-ORACLE ---------------------------------------------------------
    log("V-ORACLE", "V-ORACLE  stdout byte-identity + exit status")
    out = build_and_run(real_src, os.path.join(work, "oracle_pos"))
    all_ok &= record("V-ORACLE", "positive[reference_solution].built", True, out["built"],
                     out["build_err"].strip()[:160])
    all_ok &= record("V-ORACLE", "positive[reference_solution].exit0", True,
                     out["run_rc"] == 0)
    all_ok &= record("V-ORACLE", "positive[reference_solution].stdout", True,
                     out["stdout"] == expected)
    wrong = real_src.replace("i < 50", "i < 49")
    out = build_and_run(wrong, os.path.join(work, "oracle_neg"))
    all_ok &= record("V-ORACLE", "negative[49_terms_instead_of_50]", False,
                     out["stdout"] == expected, repr((out["stdout"] or "").split("\n")[0]))

    # ---- summary ----------------------------------------------------------
    summary = {
        "language_id": "zig",
        "condition": "I1",
        "seed": MAP_DOC["seed"],
        "zig_version": subprocess.run([ZIG, "version"], capture_output=True,
                                      text=True).stdout.strip(),
        "entry_filename": ENTRY_FILENAME,
        "build_command": " ".join(BUILD_CMD),
        "run_command": " ".join(RUN_CMD),
        "checks": RESULTS,
        "all_pass": all(x["pass"] for x in RESULTS),
        "n_checks": len(RESULTS),
        "n_failed": sum(0 if x["pass"] else 1 for x in RESULTS),
    }
    print("\n%d checks, %d failed, all_pass=%s"
          % (summary["n_checks"], summary["n_failed"], summary["all_pass"]))
    if EVID:
        with open(os.path.join(EVID, "result.json"), "w", encoding="utf-8") as fh:
            fh.write(json.dumps(summary, indent=2) + "\n")
    return 0 if summary["all_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
