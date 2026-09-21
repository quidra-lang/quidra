#!/usr/bin/env python3
"""
Correctness gate for the eleven micro workloads.

Applies the frozen tolerance from methodology 06 section 2.4:

    PASS iff |measured - expected| <= max(1e-9 * |expected|, 1e-12)

Integer fields and the explicitly exact fields (checksums) must match exactly
as written. Correctness gates timing: nothing is timed until this passes
(methodology 06 section 5.1).
"""

from __future__ import annotations

import argparse
import math
import os
import re
import sys

REL = 1e-9
ABS_FLOOR = 1e-12

# A token is either name=value or a bare value; floats are recognised by having
# a '.', 'e' or 'E' in them.
_FLOATISH = re.compile(r"^[-+]?(\d+\.\d*|\.\d+|\d+)([eE][-+]?\d+)?$")


def _is_float_literal(s):
    return bool(_FLOATISH.match(s)) and (("." in s) or ("e" in s) or ("E" in s))


def parse_line(line):
    """Return (tag, fields) where fields is a list of (name|None, text)."""
    parts = line.split()
    if not parts:
        return None, []
    tag, rest = parts[0], parts[1:]
    fields = []
    for p in rest:
        if "=" in p:
            n, v = p.split("=", 1)
            fields.append((n, v))
        else:
            fields.append((None, p))
    return tag, fields


def compare(expected_text, actual_text, require_all=False):
    # Comment lines (provenance notes, reference timings) are not results.
    def _content(text):
        return [l for l in text.strip().splitlines()
                if l.strip() and not l.strip().startswith("#")]

    exp_lines = _content(expected_text)
    act_lines = _content(actual_text)
    results, ok_all = [], True

    exp_by_tag = {}
    for l in exp_lines:
        t, f = parse_line(l)
        if t:
            exp_by_tag[t] = f
    act_by_tag = {}
    for l in act_lines:
        t, f = parse_line(l)
        if t:
            act_by_tag[t] = f

    # Each workload is implemented as its own single-workload program, so a given
    # output normally covers ONE of the eleven expected lines. Missing lines are
    # therefore "not covered by this output" rather than a failure -- unless
    # --require-all is given, which is used for the whole-suite check.
    #
    # Soundness guard: an EMPTY intersection is always a failure. Without that,
    # a program printing nothing would pass vacuously, and a gate that cannot
    # fail measures nothing (spec 10.4 pre-flight requirement (c)).
    covered = [t for t in exp_by_tag if t in act_by_tag]
    if not covered:
        return False, [{"workload": "(none)", "ok": False,
                        "reason": "output covered no expected workload line at all"}]

    for tag, exp_fields in exp_by_tag.items():
        if tag not in act_by_tag:
            if require_all:
                results.append({"workload": tag, "ok": False, "reason": "missing from output"})
                ok_all = False
            else:
                results.append({"workload": tag, "ok": True, "skipped": True,
                                "reason": "not covered by this output"})
            continue
        act_fields = act_by_tag[tag]
        if len(act_fields) != len(exp_fields):
            results.append({"workload": tag, "ok": False,
                            "reason": f"field count {len(act_fields)} != expected {len(exp_fields)}"})
            ok_all = False
            continue

        field_res, ok = [], True
        for i, ((en, ev), (an, av)) in enumerate(zip(exp_fields, act_fields)):
            name = en or f"#{i}"
            if en is not None and an is not None and en != an:
                field_res.append({"field": name, "ok": False,
                                  "reason": f"field name {an!r} != expected {en!r}"})
                ok = False
                continue
            if _is_float_literal(ev):
                try:
                    e, a = float(ev), float(av)
                except ValueError:
                    field_res.append({"field": name, "ok": False,
                                      "reason": f"not numeric: {av!r}"})
                    ok = False
                    continue
                if math.isnan(e) and math.isnan(a):
                    field_res.append({"field": name, "ok": True, "note": "both NaN"})
                    continue
                tol = max(REL * abs(e), ABS_FLOOR)
                d = abs(a - e)
                good = d <= tol
                field_res.append({"field": name, "ok": good, "expected": ev, "actual": av,
                                  "abs_diff": d, "tolerance": tol})
                ok = ok and good
            else:
                # integer / checksum / exact string field
                good = ev.strip() == av.strip()
                field_res.append({"field": name, "ok": good, "expected": ev, "actual": av,
                                  "comparison": "exact"})
                ok = ok and good
        results.append({"workload": tag, "ok": ok, "fields": field_res})
        ok_all = ok_all and ok

    for tag in act_by_tag:
        if tag not in exp_by_tag:
            results.append({"workload": tag, "ok": False, "reason": "unexpected extra output line"})
            ok_all = False

    return ok_all, results


def format_report(ok_all, results, verbose=False):
    out = []
    for r in results:
        if r.get("skipped") and not verbose:
            continue
        mark = "SKIP" if r.get("skipped") else ("PASS" if r["ok"] else "FAIL")
        out.append(f"{mark} {r['workload']}" + (f"  ({r['reason']})" if r.get("reason") else ""))
        for f in r.get("fields", []):
            if not f["ok"] or verbose:
                m = "  ok " if f["ok"] else "  BAD"
                if "abs_diff" in f:
                    out.append(f"{m} {f['field']}: expected {f['expected']} actual {f['actual']} "
                               f"|d|={f['abs_diff']:.3e} tol={f['tolerance']:.3e}")
                else:
                    out.append(f"{m} {f['field']}: expected {f.get('expected')!r} "
                               f"actual {f.get('actual')!r} {f.get('reason','')}")
    out.append("")
    out.append("OVERALL: " + ("PASS — correctness gate satisfied"
                              if ok_all else "FAIL — must be repaired before any timing run"))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expected", required=True, help="golden output file")
    ap.add_argument("--actual", required=True, help="candidate output file (or - for stdin)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--require-all", action="store_true",
                    help="every expected workload line must be present (whole-suite check)")
    a = ap.parse_args()
    exp = open(a.expected).read()
    act = sys.stdin.read() if a.actual == "-" else open(a.actual).read()
    ok, res = compare(exp, act, require_all=a.require_all)
    print(format_report(ok, res, a.verbose))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
