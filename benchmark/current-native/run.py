#!/usr/bin/env python3
"""Current-Quidra micro diagnostic layered on the frozen 2026-09-17 suite."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FROZEN = os.path.join(ROOT, "2026-09-17-7677581(latest)")
SCRIPTS = os.path.join(FROZEN, "scripts")
sys.path.insert(0, SCRIPTS)

import langs  # noqa: E402
import measure  # noqa: E402
import check_micro  # noqa: E402

FROZEN_SRC = os.path.join(FROZEN, "micro", "src")
OVERLAY = os.path.join(HERE, "quidra")
GOLDEN = os.path.join(FROZEN, "micro", "reference", "golden_c.txt")
WORKLOADS = [f"mb{n:02d}" for n in range(1, 12)]
CONFIGS = [
    ("quidra_native", "quidra", "quidra"),
    ("quidra_compile_execute", "quidra_interp", "quidra"),
    ("cpp", "cpp", "cpp"),
    ("rust", "rust", "rust"),
    ("go", "go", "go"),
]


def source_for(target: str, srcdir: str, wl: str) -> str:
    ext = langs.TARGETS[target]["ext"]
    if srcdir == "quidra":
        current = os.path.join(OVERLAY, wl + ext)
        if os.path.exists(current):
            return current
    return os.path.join(FROZEN_SRC, srcdir, wl + ext)


def load_golden_line(wl: str) -> str | None:
    tag = "MB" + wl[2:]
    with open(GOLDEN) as f:
        for line in f:
            if line.strip().startswith(tag + " "):
                return line.rstrip("\n")
    return None


def run_one(cfg_id, target, srcdir, wl, repeats, warmups, timeout, verify_only, build_root):
    src = source_for(target, srcdir, wl)
    if not os.path.exists(src):
        return {"config": cfg_id, "workload": wl, "status": "missing_source", "source": src}

    wdir = os.path.join(build_root, cfg_id, wl)
    os.makedirs(wdir, exist_ok=True)
    stem = wl
    rec = {"config": cfg_id, "workload": wl, "source": os.path.relpath(src, ROOT)}

    t0 = time.perf_counter()
    try:
        built = langs.build(target, src, wdir, stem, timeout=timeout)
    except Exception as exc:  # noqa: BLE001
        rec.update(status="build_error", error=str(exc)[:500])
        return rec
    rec["build"] = {
        "needed": built["needs_build"],
        "ok": built["ok"],
        "cmd": built.get("cmd"),
        "wall_seconds": round(time.perf_counter() - t0, 4),
        "diagnostics": ((built.get("stderr") or "") + (built.get("stdout") or ""))[:2000],
    }
    if not built["ok"]:
        rec["status"] = "build_failed"
        return rec

    rec["artifact_size_bytes"] = langs.artifact_size(target, wdir, stem, src)
    cmd = langs.run_cmd(target, src, wdir, stem, main_class="Main")
    try:
        process = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
            env=langs.env_for(target), cwd=wdir)
    except subprocess.TimeoutExpired:
        rec["status"] = "run_timeout"
        return rec

    rec["run_exit_code"] = process.returncode
    rec["stdout"] = process.stdout.strip()[:2000]
    rec["stderr"] = process.stderr.strip()[:2000]
    if process.returncode != 0:
        rec["status"] = "run_failed"
        return rec

    expected = load_golden_line(wl)
    ok, detail = check_micro.compare(expected, process.stdout)
    rec["correct"] = ok
    rec["correctness_detail"] = detail
    if not ok:
        rec["status"] = "silent_bug"
        return rec
    if verify_only:
        rec["status"] = "verified"
        return rec

    measured = measure.measure(
        cmd, repeats=repeats, warmups=warmups, cwd=wdir,
        timeout=timeout, label=f"{cfg_id}/{wl}")
    rec["timing"] = {
        "repeats": repeats,
        "warmups": warmups,
        "wall_seconds": measured["wall_seconds"],
        "cpu_seconds": measured["cpu_seconds"],
        "peak_rss_bytes": measured["peak_rss_bytes"],
        "first_run_wall_seconds": measured["first_run_wall_seconds"],
        "warmup_wall_seconds": measured["warmup_wall_seconds"],
        "representative": measured["representative"],
    }
    rec["status"] = "ok"
    return rec


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--configs", default="quidra_native,quidra_compile_execute,cpp,rust,go")
    parser.add_argument("--workloads", default="")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    configs = [c for c in CONFIGS if c[0] in args.configs.split(",")]
    workloads = [w for w in WORKLOADS if not args.workloads or w in args.workloads.split(",")]
    results = []
    with tempfile.TemporaryDirectory(prefix="quidra-current-native-") as build_root:
        for cfg_id, target, srcdir in configs:
            for wl in workloads:
                result = run_one(
                    cfg_id, target, srcdir, wl, args.repeats, args.warmups,
                    args.timeout, args.verify_only, build_root)
                results.append(result)
                suffix = ""
                if result.get("timing"):
                    suffix = f"  {result['timing']['representative']['wall_seconds']:.3f}s"
                print(f"{cfg_id:24s} {wl}  {result.get('status')}{suffix}", flush=True)

    with open(args.out, "w") as f:
        json.dump({
            "kind": "current-equivalent-diagnostic",
            "frozen_suite": "2026-09-17-7677581(latest)",
            "overlays": ["mb03", "mb08", "mb10", "mb11"],
            "repeats": args.repeats,
            "warmups": args.warmups,
            "results": results,
        }, f, indent=1)

    bad = [r for r in results if r.get("status") not in ("ok", "verified")]
    print(f"\n{len(results)-len(bad)}/{len(results)} passed; wrote {args.out}")
    for result in bad[:40]:
        print(f"  {result['config']:24s} {result['workload']} {result.get('status')}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
