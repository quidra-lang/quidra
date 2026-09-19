#!/usr/bin/env python3
"""WL-SVM field-by-field comparator under frozen tolerance ALGO-TOL-1.

Usage:  compare_svm.py <actual> <golden>

Implements methodology/07_algorithm_workloads.md section 2.7 (schema),
section 2.6 (invariants) and frozen_tolerance.json ALGO-TOL-1:

  numeric_match: abs(a-g) <= max( abs + rel*abs(g), print_ulp[kind] )
                 abs = 1e-9, rel = 1e-6
  print_ulp:     F6 = 1e-6, F12 = 1e-12
  discrete:      exact equality

Exit 0 on PASS, 1 on FAIL.
"""

import sys

ABS = 1e-9
REL = 1e-6
PRINT_ULP = {"F6": 1e-6, "F12": 1e-12}

C_REG = 10.0
N = 200
N_TEST = 100

# Schema: line key -> (kind, count).  kind in {INT, F6, F12}
SCHEMA = [
    ("SVM_VERSION",    "INT",  1),
    ("SWEEPS",         "INT",  1),
    ("CONVERGED",      "INT",  1),
    ("ERROR_LAST",     "F6",   1),
    ("MAX_ABS_DELTA",  "F6",   1),
    ("BETA",           "F6",   1),
    ("NS_MARGIN",      "INT",  1),
    ("NS_INSIDE",      "INT",  1),
    ("W",              "F6",   4),
    ("B",              "F6",   1),
    ("OBJECTIVE",      "F6",   1),
    ("ALPHA_SUM",      "F6",   1),
    ("ALPHA_Y_SUM",    "F12",  1),
    ("ALPHA_CHECKSUM", "F6",   1),
    ("TRAIN_ACC",      "F6",   1),
    ("TEST_ACC",       "F6",   1),
    ("TEST_ACC_C1",    "F6",   1),
    ("TEST_ACC_C2",    "F6",   1),
    ("TEST_CORRECT",   "INT",  3),
    ("PRED",           "INT",  100),
    ("ALPHA",          "F6",   200),
]


def parse(path):
    with open(path) as fh:
        raw = fh.read()
    lines = raw.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) != len(SCHEMA):
        raise ValueError("expected %d lines, got %d" % (len(SCHEMA), len(lines)))
    out = {}
    for idx, (key, kind, count) in enumerate(SCHEMA):
        parts = lines[idx].split()
        if parts[0] != key:
            raise ValueError("line %d: expected key %r, got %r" % (idx + 1, key, parts[0]))
        vals = parts[1:]
        if len(vals) != count:
            raise ValueError("line %d (%s): expected %d values, got %d"
                             % (idx + 1, key, count, len(vals)))
        out[key] = (kind, vals)
    return out


def compare(actual, golden):
    """Return (ok, max_abs_diff, max_rel_err, failures)."""
    failures = []
    max_abs = 0.0
    max_relerr = 0.0  # spec's numerical_error_definition: |a-g| / (1+|g|)

    for key, kind, count in SCHEMA:
        akind, avals = actual[key]
        gkind, gvals = golden[key]
        for k in range(count):
            av, gv = avals[k], gvals[k]
            label = key if count == 1 else "%s[%d]" % (key, k)
            if kind == "INT":
                if int(av) != int(gv):
                    failures.append("%s: discrete mismatch %s != %s" % (label, av, gv))
            else:
                a, g = float(av), float(gv)
                d = abs(a - g)
                if d > max_abs:
                    max_abs = d
                re = d / (1.0 + abs(g))
                if re > max_relerr:
                    max_relerr = re
                bound = max(ABS + REL * abs(g), PRINT_ULP[kind])
                if d > bound:
                    failures.append("%s: |%.17g - %.17g| = %.3e > bound %.3e"
                                    % (label, a, g, d, bound))
    return (len(failures) == 0), max_abs, max_relerr, failures


def invariants(doc):
    """Section 2.6 implementation-independent invariants."""
    res = []
    ok = True

    ays = float(doc["ALPHA_Y_SUM"][1][0])
    c = abs(ays) <= 1.0
    ok &= c
    res.append(("|ALPHA_Y_SUM| <= 1.0", "%.12f" % abs(ays), c))

    alphas = [float(v) for v in doc["ALPHA"][1]]
    c = all(0.0 <= a <= C_REG for a in alphas)
    ok &= c
    res.append(("0 <= alpha[i] <= C for all i",
                "min=%.6f max=%.6f" % (min(alphas), max(alphas)), c))

    nsm = int(doc["NS_MARGIN"][1][0])
    nsi = int(doc["NS_INSIDE"][1][0])
    c = (nsm + nsi <= N) and (nsm >= 1)
    ok &= c
    res.append(("NS_MARGIN+NS_INSIDE <= N and NS_MARGIN >= 1",
                "%d+%d=%d, N=%d" % (nsm, nsi, nsm + nsi, N), c))

    ta = float(doc["TEST_ACC"][1][0])
    c = abs(ta * 100.0 - round(ta * 100.0)) < 1e-9
    ok &= c
    res.append(("TEST_ACC*100 is an integer count", "%.6f -> %g" % (ta, ta * 100), c))

    preds = [int(v) for v in doc["PRED"][1]]
    c = (len(preds) == N_TEST) and all(p in (1, -1) for p in preds)
    ok &= c
    res.append(("PRED has 100 entries, each +1 or -1",
                "n=%d, set=%s" % (len(preds), sorted(set(preds))), c))

    # Cross-checks against NS_MARGIN / NS_INSIDE recomputed from ALPHA
    EPS = 1e-7
    rec_m = sum(1 for a in alphas if EPS < a < C_REG - EPS)
    rec_i = sum(1 for a in alphas if a >= C_REG - EPS)
    c = (rec_m == nsm) and (rec_i == nsi)
    ok &= c
    res.append(("NS_MARGIN/NS_INSIDE consistent with printed ALPHA",
                "recomputed %d/%d vs printed %d/%d" % (rec_m, rec_i, nsm, nsi), c))

    # ALPHA_SUM / ALPHA_CHECKSUM consistency with printed ALPHA (F6 rounding
    # means this is only accurate to ~200*5e-7 = 1e-4; use a loose bound).
    s = sum(alphas)
    printed = float(doc["ALPHA_SUM"][1][0])
    c = abs(s - printed) < 1e-3
    ok &= c
    res.append(("ALPHA_SUM consistent with printed ALPHA vector",
                "sum(printed alpha)=%.6f vs ALPHA_SUM=%.6f" % (s, printed), c))

    chk = sum(alphas[i] * float((i % 97) + 1) for i in range(len(alphas)))
    printed_chk = float(doc["ALPHA_CHECKSUM"][1][0])
    c = abs(chk - printed_chk) < 1e-1
    ok &= c
    res.append(("ALPHA_CHECKSUM consistent with printed ALPHA vector",
                "recomputed=%.6f vs printed=%.6f" % (chk, printed_chk), c))

    # TEST_CORRECT consistent with PRED
    cc1 = sum(1 for p in preds[:50] if p == 1)
    cc2 = sum(1 for p in preds[50:] if p == -1)
    tc = [int(v) for v in doc["TEST_CORRECT"][1]]
    c = (tc == [cc1, cc2, cc1 + cc2])
    ok &= c
    res.append(("TEST_CORRECT consistent with PRED",
                "from PRED %d %d %d vs printed %s" % (cc1, cc2, cc1 + cc2, tc), c))

    return ok, res


def main():
    actual_path, golden_path = sys.argv[1], sys.argv[2]
    try:
        actual = parse(actual_path)
    except ValueError as exc:
        print("OUTPUT_CONTRACT_FAIL (actual): %s" % exc)
        return 1
    golden = parse(golden_path)

    ok, max_abs, max_relerr, failures = compare(actual, golden)
    inv_ok, inv_res = invariants(actual)

    print("actual : %s" % actual_path)
    print("golden : %s" % golden_path)
    print("")
    print("max abs difference over all F6/F12 fields : %.17g" % max_abs)
    print("numerical error  max |a-g|/(1+|g|)        : %.17g" % max_relerr)
    print("tolerance ALGO-TOL-1 : abs=%g rel=%g, print_ulp F6=%g F12=%g"
          % (ABS, REL, PRINT_ULP["F6"], PRINT_ULP["F12"]))
    print("")
    print("section 2.6 invariants:")
    for name, detail, c in inv_res:
        print("  [%s] %-52s %s" % ("ok" if c else "FAIL", name, detail))
    print("")
    if failures:
        print("FIELD FAILURES (%d):" % len(failures))
        for f in failures[:40]:
            print("  " + f)
        if len(failures) > 40:
            print("  ... and %d more" % (len(failures) - 40))
    verdict = ok and inv_ok
    print("")
    print("VERDICT: %s" % ("PASS" if verdict else "FAIL"))
    return 0 if verdict else 1


if __name__ == "__main__":
    sys.exit(main())
