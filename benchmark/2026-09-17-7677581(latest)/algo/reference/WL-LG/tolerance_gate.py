#!/usr/bin/env python3
"""
WL-LG golden gate: applies ALGO-TOL-1 (methodology/frozen_tolerance.json) to a
candidate stdout against golden/expected_stdout.txt.

Usage: tolerance_gate.py <candidate.txt> [golden.txt]
Exit:  0 = PASS, 1 = REJECT (SILENT_BUG), 2 = OUTPUT_CONTRACT_FAIL

Field kinds for WL-LG (spec 4.8): LIGHTGRAD_VERSION, STRESS_STEPS and NODE_TYPES
are INT (exact equality); all remaining 141 values are F6.
"""
import sys, json, os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__),
                                    "..", "..", "..", "methodology"))
TOL = json.load(open(os.path.join(ROOT, "frozen_tolerance.json")))
NUM, ULP = TOL["numeric_match"], TOL["print_ulp"]
INT_KEYS = {"LIGHTGRAD_VERSION", "STRESS_STEPS", "NODE_TYPES"}

SCHEMA = [("LIGHTGRAD_VERSION",1),("SCALAR_Y",1),("SCALAR_DY_DX1",1),("SCALAR_D2Y_DX1",1),
  ("SCALAR_D3Y_DX1",1),("SCALAR_D4Y_DX1",1),("SCALAR_DY_DX2",1),("SCALAR_D2Y_DX2",1),
  ("SCALAR_DY_DX3",1),("SCALAR_D2Y_DX3",1),("SCALAR_D2Y_DX1DX2",1),("TENSOR_B1",12),
  ("TENSOR_B2",12),("TENSOR_B3",12),("TENSOR_C",12),("TENSOR_D",1),("GRAD_A1",12),
  ("GRAD_A2",12),("GRAD_A3",12),("GRAD_A4",1),("NEW_A1",12),("NEW_A2",12),("NEW_A3",12),
  ("NEW_A4",1),("STRESS_STEPS",1),("STRESS_GRAD_P0",1),("STRESS_GRAD_Q0",1),
  ("STRESS_LOSS_0",1),("STRESS_LOSS_50",1),("STRESS_LOSS_100",1),("STRESS_LOSS_FINAL",1),
  ("STRESS_P_SUM",1),("STRESS_Q_SUM",1),("NODE_TYPES",1)]

def parse(path, strict):
    raw = open(path, "rb").read().decode("ascii")
    if b"\r" in open(path, "rb").read():
        raise ValueError("CR in output; contract requires LF only (1.4)")
    if not raw.endswith("\n"):
        raise ValueError("missing trailing newline (1.4)")
    lines = raw[:-1].split("\n")
    if strict and len(lines) != len(SCHEMA):
        raise ValueError(f"expected {len(SCHEMA)} lines, got {len(lines)}")
    fields = []
    for idx, ln in enumerate(lines):
        if strict:
            key, count = SCHEMA[idx]
            toks = ln.split(" ")
            if toks[0] != key:
                raise ValueError(f"line {idx+1}: expected key {key}, got {toks[0]}")
            if len(toks) - 1 != count:
                raise ValueError(f"line {idx+1} ({key}): expected {count} values, got {len(toks)-1}")
            if ln != ln.strip():
                raise ValueError(f"line {idx+1}: leading/trailing whitespace")
            for v in toks[1:]:
                low = v.lower()
                if "nan" in low or "inf" in low:
                    raise ValueError(f"line {idx+1} ({key}): non-finite token {v!r}")
                if key not in INT_KEYS:
                    if v.startswith("-0.000000") or v == "-0.000000":
                        raise ValueError(f"line {idx+1} ({key}): negative zero forbidden")
                    frac = v.split(".")
                    if len(frac) != 2 or len(frac[1]) != 6:
                        raise ValueError(f"line {idx+1} ({key}): {v!r} is not F6")
                fields.append((f"{key}[{len(fields)}]", key, v))
        else:
            toks = ln.split(" ")
            for v in toks[1:]:
                fields.append((f"{toks[0]}", toks[0], v))
    return fields

def main():
    cand, gold = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else
        os.path.join(os.path.dirname(__file__), "golden", "expected_stdout.txt"))
    try:
        a = parse(cand, True)
        b = parse(gold, True)
    except ValueError as e:
        print(f"OUTPUT_CONTRACT_FAIL: {e}")
        return 2
    fails, worst, worstf = [], 0.0, None
    for (fa, ka, va), (_, kb, vb) in zip(a, b):
        if ka in INT_KEYS:
            if va != vb:
                fails.append((fa, va, vb, "discrete field differs"))
            continue
        x, g = float(va), float(vb)
        d = abs(x - g)
        r = d / (1.0 + abs(g))
        if r > worst: worst, worstf = r, fa
        if not (d <= max(NUM["abs"] + NUM["rel"] * abs(g), ULP["F6"])):
            fails.append((fa, va, vb, f"|d|={d:.6g} exceeds band"))
    print(f"fields: {len(a)}   Numerical Error: {worst:.6g} ({worstf})   failing: {len(fails)}")
    for f in fails[:10]:
        print(f"  FIELD {f[0]}  actual={f[1]}  golden={f[2]}  {f[3]}")
    if fails:
        print("REJECT (SILENT_BUG)")
        return 1
    print("PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())
