#!/usr/bin/env python3
"""
Serial micro-benchmark suite runner: build -> correctness gate -> timing.

Correctness gates timing (methodology 06 section 5.1): nothing is timed until it
has reproduced the golden output. A configuration that fails the gate is recorded
as a failure and contributes no timing, rather than contributing a fast wrong
answer.

MUST be run with no agent fan-out active. Timing on a loaded host is not a
measurement of the language.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import langs      # noqa: E402
import measure    # noqa: E402
import check_micro  # noqa: E402

RUN = os.path.dirname(D)
SRC = os.path.join(RUN, "micro", "src")
GOLDEN = os.path.join(RUN, "micro", "reference", "golden_c.txt")
OUT = os.path.join(RUN, "micro", "raw")
BUILD = os.path.join(RUN, "micro", "build")

# Execution configurations. Quidra appears twice -- both modes run the SAME
# .qui source, differing only in invocation (spec section 11).
CONFIGS = [
    ("quidra_native", "quidra", "quidra"),
    ("quidra_interpreter", "quidra_interp", "quidra"),
    ("python", "python", "python"),
    ("cpp", "cpp", "cpp"),
    ("rust", "rust", "rust"),
    ("go", "go", "go"),
    ("java", "java", "java"),
    ("typescript", "typescript", "typescript"),
    ("kotlin", "kotlin", "kotlin"),
    ("swift", "swift", "swift"),
    ("zig", "zig", "zig"),
]

WORKLOADS = [f"mb{n:02d}" for n in range(1, 12)]


def load_golden_line(wl):
    """The single expected line for this workload, e.g. 'MB01 ...'."""
    tag = "MB" + wl[2:]
    for line in open(GOLDEN):
        if line.strip().startswith(tag + " "):
            return line.rstrip("\n")
    return None


def run_one(cfg_id, target, srcdir, wl, repeats, warmups, timeout, verify_only):
    ext = langs.TARGETS[target]["ext"]
    stem = "Main" if target == "java" else wl
    src = os.path.join(SRC, srcdir, wl + ext)
    if not os.path.exists(src):
        return {"config": cfg_id, "workload": wl, "status": "missing_source", "source": src}

    wdir = os.path.join(BUILD, cfg_id, wl)
    os.makedirs(wdir, exist_ok=True)

    # Java needs the file named Main.java next to its class.
    build_src = src
    if target == "java":
        build_src = os.path.join(wdir, "Main.java")
        text = open(src).read()
        shutil.copyfile(src, build_src) if "class Main" in text else open(build_src, "w").write(text)

    rec = {"config": cfg_id, "workload": wl, "source": src}

    # ---- build -----------------------------------------------------------
    t0 = time.perf_counter()
    try:
        b = langs.build(target, build_src, wdir, stem, timeout=timeout)
    except Exception as e:  # noqa: BLE001
        rec.update(status="build_error", error=str(e)[:500])
        return rec
    rec["build"] = {
        "needed": b["needs_build"], "ok": b["ok"], "cmd": b.get("cmd"),
        "wall_seconds": round(time.perf_counter() - t0, 4),
        "diagnostics": ((b.get("stderr") or "") + (b.get("stdout") or ""))[:2000],
    }
    if not b["ok"]:
        rec["status"] = "build_failed"
        return rec

    rec["artifact_size_bytes"] = langs.artifact_size(target, wdir, stem, build_src)

    # ---- correctness gate (always, before any timing) --------------------
    cmd = langs.run_cmd(target, build_src, wdir, stem, main_class="Main")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           env=langs.env_for(target), cwd=wdir)
    except subprocess.TimeoutExpired:
        rec["status"] = "run_timeout"
        return rec
    rec["run_exit_code"] = p.returncode
    rec["stdout"] = p.stdout.strip()[:2000]
    rec["stderr"] = p.stderr.strip()[:2000]
    if p.returncode != 0:
        rec["status"] = "run_failed"
        return rec

    expected = load_golden_line(wl)
    if expected is None:
        rec["status"] = "no_golden_line"
        return rec
    ok, detail = check_micro.compare(expected, p.stdout)
    rec["correct"] = ok
    rec["correctness_detail"] = detail
    if not ok:
        # Built, ran cleanly, wrong answer: a silent bug, not a success.
        rec["status"] = "silent_bug"
        return rec

    if verify_only:
        rec["status"] = "verified"
        return rec

    # ---- timing (only after the gate passes) -----------------------------
    m = measure.measure(cmd, repeats=repeats, warmups=warmups, cwd=wdir,
                        timeout=timeout, label=f"{cfg_id}/{wl}")
    rec["timing"] = {
        "repeats": repeats, "warmups": warmups,
        "wall_seconds": m["wall_seconds"],
        "cpu_seconds": m["cpu_seconds"],
        "peak_rss_bytes": m["peak_rss_bytes"],
        "first_run_wall_seconds": m["first_run_wall_seconds"],
        "warmup_wall_seconds": m["warmup_wall_seconds"],
        "representative": m["representative"],
    }
    rec["status"] = "ok"
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--configs", default="", help="comma-separated config ids (default: all)")
    ap.add_argument("--workloads", default="", help="comma-separated workloads (default: all)")
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--warmups", type=int, default=2,
                    help="warm-up runs, excluded from the representative statistic")
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--verify-only", action="store_true",
                    help="build and check correctness, do not time (safe while agents run)")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    cfgs = [c for c in CONFIGS if not a.configs or c[0] in a.configs.split(",")]
    wls = [w for w in WORKLOADS if not a.workloads or w in a.workloads.split(",")]

    os.makedirs(OUT, exist_ok=True)
    results = []
    for cfg_id, target, srcdir in cfgs:
        for wl in wls:
            r = run_one(cfg_id, target, srcdir, wl, a.repeats, a.warmups,
                        a.timeout, a.verify_only)
            results.append(r)
            st = r.get("status")
            t = ""
            if r.get("timing"):
                t = f"  {r['timing']['representative']['wall_seconds']:.3f}s"
            print(f"{cfg_id:20s} {wl}  {st}{t}", flush=True)

    out = a.out or os.path.join(OUT, "micro_verify.json" if a.verify_only else "micro_timing.json")
    with open(out, "w") as f:
        json.dump({"mode": "verify" if a.verify_only else "timing",
                   "repeats": a.repeats, "warmups": a.warmups,
                   "results": results}, f, indent=1)

    bad = [r for r in results if r.get("status") not in ("ok", "verified")]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed; wrote {out}")
    if bad:
        print("NOT OK:")
        for r in bad[:40]:
            print(f"  {r['config']:20s} {r['workload']}  {r.get('status')}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
