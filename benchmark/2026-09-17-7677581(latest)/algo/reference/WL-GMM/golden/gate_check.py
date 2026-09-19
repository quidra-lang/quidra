#!/usr/bin/env python3
"""WL-GMM golden gate: field-by-field comparison under frozen tolerance ALGO-TOL-1.

Usage: gate.py <actual> <golden>
Exit 0 = PASS, 1 = FAIL.
"""
import json, sys, math

TOL = json.load(open("/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581/methodology/frozen_tolerance.json"))
ABS = TOL["numeric_match"]["abs"]          # 1e-9
REL = TOL["numeric_match"]["rel"]          # 1e-6
ULP = TOL["print_ulp"]                     # F6 -> 1e-6, F12 -> 1e-12

# Field kind map per spec 3.8. I=discrete (exact equality), 6=F6, 12=F12.
SCHEMA = {
    "GMM_VERSION":       ["I"],
    "ITERATIONS":        ["I"],
    "CONVERGED":         ["I"],
    "LOGLIK_INIT":       ["6"],
    "LOGLIK":            ["6"],
    "LOGLIK_PER_POINT":  ["6"],
    "DELTA_LOGLIK_LAST": ["12"],
    "MONOTONE":          ["I"],
    # COMP <k> PI <F6> MU <F6>x3 SIGMA <F6>x9  -> handled specially
    "ASSIGN_COUNTS":     ["I"] * 4,
    "ASSIGN_CHECKSUM":   ["I"],
    "ASSIGN_FIRST_20":   ["I"] * 20,
    "GAMMA_ROW_DEV_MAX": ["12"],
    "PI_SUM_DEV":        ["12"],
    "ASSIGN_MIN_MARGIN": ["12"],
}
ORDER = ["GMM_VERSION","ITERATIONS","CONVERGED","LOGLIK_INIT","LOGLIK",
         "LOGLIK_PER_POINT","DELTA_LOGLIK_LAST","MONOTONE",
         "COMP0","COMP1","COMP2","COMP3",
         "ASSIGN_COUNTS","ASSIGN_CHECKSUM","ASSIGN_FIRST_20",
         "GAMMA_ROW_DEV_MAX","PI_SUM_DEV","ASSIGN_MIN_MARGIN"]

def parse(path):
    lines = open(path).read().split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) != 18:
        raise SystemExit("OUTPUT_CONTRACT_FAIL %s: %d lines, expected 18" % (path, len(lines)))
    rec = {}
    for ln in lines:
        t = ln.split()
        if t[0] == "COMP":
            k = int(t[1])
            if t[2] != "PI" or t[6] != "MU" and False: pass
            # literal tokens must appear exactly as specified
            if t[2] != "PI" or t[4] != "MU" or t[8] != "SIGMA":
                raise SystemExit("OUTPUT_CONTRACT_FAIL %s: bad COMP tokens: %s" % (path, ln))
            vals = [("I", t[1]), ("6", t[3])] + [("6", v) for v in t[5:8]] + [("6", v) for v in t[9:18]]
            if len(t) != 18:
                raise SystemExit("OUTPUT_CONTRACT_FAIL %s: COMP has %d tokens" % (path, len(t)))
            rec["COMP%d" % k] = vals
        else:
            key = t[0]
            if key not in SCHEMA:
                raise SystemExit("OUTPUT_CONTRACT_FAIL %s: unknown field %s" % (path, key))
            kinds = SCHEMA[key]
            if len(t) - 1 != len(kinds):
                raise SystemExit("OUTPUT_CONTRACT_FAIL %s: %s arity %d != %d" % (path, key, len(t)-1, len(kinds)))
            rec[key] = list(zip(kinds, t[1:]))
    missing = [k for k in ORDER if k not in rec]
    if missing:
        raise SystemExit("OUTPUT_CONTRACT_FAIL %s: missing %s" % (path, missing))
    return rec

def main():
    a_path, g_path = sys.argv[1], sys.argv[2]
    A, G = parse(a_path), parse(g_path)
    max_abs = 0.0; max_abs_field = None
    max_err = 0.0; max_err_field = None   # numerical_error_definition
    fails = []; ncmp = 0
    for key in ORDER:
        for i, ((ka, va), (kg, vg)) in enumerate(zip(A[key], G[key])):
            ncmp += 1
            name = "%s[%d]" % (key, i)
            if ka == "I":
                if va != vg:
                    fails.append("%s discrete mismatch: actual=%s golden=%s" % (name, va, vg))
                continue
            fa, fg = float(va), float(vg)
            d = abs(fa - fg)
            bound = max(ABS + REL * abs(fg), ULP["F6" if ka == "6" else "F12"])
            if d > max_abs: max_abs, max_abs_field = d, name
            e = d / (1.0 + abs(fg))
            if e > max_err: max_err, max_err_field = e, name
            if d > bound:
                fails.append("%s numeric out of tolerance: |%.17g - %.17g| = %.3e > %.3e"
                             % (name, fa, fg, d, bound))
    print("fields compared      : %d" % ncmp)
    print("max abs difference   : %.17g  (%s)" % (max_abs, max_abs_field))
    print("numerical error      : %.17g  (%s)" % (max_err, max_err_field))
    print("byte-identical       : %s" % (open(a_path,'rb').read() == open(g_path,'rb').read()))
    if fails:
        print("RESULT: FAIL (%d field(s) outside ALGO-TOL-1)" % len(fails))
        for f in fails[:20]: print("  - " + f)
        return 1
    print("RESULT: PASS (every field inside ALGO-TOL-1)")
    return 0

sys.exit(main())
