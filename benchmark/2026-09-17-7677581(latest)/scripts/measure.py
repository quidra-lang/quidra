#!/usr/bin/env python3
"""
Quidra benchmark measurement harness.

Frozen measurement primitives for the Standard evaluation. Captures wall-clock
time, CPU time (user+sys) and peak resident set size for a command, repeated N
times, preserving every individual run plus dispersion. The representative raw
value is the MEDIAN, per spec section 25.2.

Peak RSS comes from macOS `/usr/bin/time -l`, whose "maximum resident set size"
field is reported in BYTES on macOS (it is kilobytes on Linux; this harness is
macOS-only and asserts the platform).

Nothing here normalizes or scores. It only produces raw measurements.
"""

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time

TIME_BIN = "/usr/bin/time"

_RE_REAL = re.compile(r"([0-9.]+)\s+real\s+([0-9.]+)\s+user\s+([0-9.]+)\s+sys")
_RE_MAXRSS = re.compile(r"([0-9]+)\s+maximum resident set size")


def _assert_platform() -> None:
    if platform.system() != "Darwin":
        raise SystemExit(
            "measure.py assumes macOS /usr/bin/time -l semantics "
            "(maximum resident set size in bytes); refusing to run elsewhere."
        )


def run_once(cmd, cwd=None, stdin_path=None, env=None, timeout=900):
    """Run `cmd` once under /usr/bin/time -l.

    Returns a dict with wall/cpu seconds, peak RSS bytes, exit code, and the
    captured stdout/stderr of the measured program (time's own output is
    stripped out of stderr).
    """
    _assert_platform()
    full = [TIME_BIN, "-l"] + list(cmd)
    merged_env = dict(os.environ)
    if env:
        merged_env.update(env)

    stdin_f = open(stdin_path, "rb") if stdin_path else subprocess.DEVNULL
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            full,
            cwd=cwd,
            stdin=stdin_f,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=merged_env,
            timeout=timeout,
        )
        timed_out = False
        rc = proc.returncode
        out = proc.stdout.decode("utf-8", "replace")
        err_raw = proc.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        timed_out = True
        rc = None
        out = ""
        err_raw = ""
    finally:
        if stdin_path:
            stdin_f.close()
    wall_outer = time.perf_counter() - t0

    wall = cpu_user = cpu_sys = None
    maxrss = None
    kept_err_lines = []
    for line in err_raw.splitlines():
        m = _RE_REAL.search(line)
        if m:
            wall, cpu_user, cpu_sys = (float(m.group(i)) for i in (1, 2, 3))
            continue
        m = _RE_MAXRSS.search(line)
        if m:
            maxrss = int(m.group(1))
            continue
        # /usr/bin/time -l emits a block of resource lines; drop them, keep the
        # program's own stderr.
        if re.match(r"^\s*[0-9]+\s+[a-z].*$", line) and "maximum resident" not in line:
            continue
        kept_err_lines.append(line)

    return {
        "cmd": list(cmd),
        "exit_code": rc,
        "timed_out": timed_out,
        "wall_seconds": wall if wall is not None else wall_outer,
        "wall_seconds_source": "time -l" if wall is not None else "python perf_counter",
        "cpu_user_seconds": cpu_user,
        "cpu_sys_seconds": cpu_sys,
        "cpu_seconds": (cpu_user + cpu_sys) if (cpu_user is not None and cpu_sys is not None) else None,
        "peak_rss_bytes": maxrss,
        "stdout": out,
        "stderr": "\n".join(kept_err_lines).strip(),
    }


def _stats(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return {
        "n": len(vals),
        "median": statistics.median(vals),
        "mean": statistics.fmean(vals),
        "min": min(vals),
        "max": max(vals),
        "stdev": statistics.stdev(vals) if len(vals) > 1 else 0.0,
        "all": vals,
    }


def measure(cmd, repeats=5, warmups=0, cwd=None, stdin_path=None, env=None,
            timeout=900, expect_stdout=None, label=None):
    """Repeat a command and summarize. Warm-up runs are recorded but excluded
    from the representative statistics, so JIT/GC runtimes can be reported both
    cold and at steady state without mixing them."""
    warm_runs = [run_once(cmd, cwd, stdin_path, env, timeout) for _ in range(warmups)]
    runs = [run_once(cmd, cwd, stdin_path, env, timeout) for _ in range(repeats)]

    failures = [r for r in runs if r["timed_out"] or r["exit_code"] != 0]
    correct = None
    if expect_stdout is not None:
        correct = all(r["stdout"].strip() == expect_stdout.strip() for r in runs)

    return {
        "label": label,
        "cmd": list(cmd),
        "repeats": repeats,
        "warmups": warmups,
        "all_runs_succeeded": not failures,
        "failure_count": len(failures),
        "output_matches_expected": correct,
        "representative": {
            "wall_seconds": _stats([r["wall_seconds"] for r in runs])["median"] if runs else None,
            "cpu_seconds": (_stats([r["cpu_seconds"] for r in runs]) or {}).get("median"),
            "peak_rss_bytes": (_stats([r["peak_rss_bytes"] for r in runs]) or {}).get("median"),
            "statistic": "median over repeats (spec 25.2)",
        },
        "wall_seconds": _stats([r["wall_seconds"] for r in runs]),
        "cpu_seconds": _stats([r["cpu_seconds"] for r in runs]),
        "peak_rss_bytes": _stats([r["peak_rss_bytes"] for r in runs]),
        "warmup_wall_seconds": _stats([r["wall_seconds"] for r in warm_runs]) if warm_runs else None,
        "first_run_wall_seconds": runs[0]["wall_seconds"] if runs else None,
        "runs": runs,
        "warmup_runs": warm_runs,
    }


def measure_build(cmd, cwd=None, env=None, timeout=1800, artifact=None, repeats=3):
    """Measure compile/build time and resulting artifact size.

    The build is repeated; caches are NOT cleared between repeats by this
    function, so callers that need cold-build numbers must remove build outputs
    themselves between calls.
    """
    runs = [run_once(cmd, cwd=cwd, env=env, timeout=timeout) for _ in range(repeats)]
    size = None
    if artifact and os.path.exists(artifact):
        if os.path.isdir(artifact):
            size = sum(
                os.path.getsize(os.path.join(dp, f))
                for dp, _, fs in os.walk(artifact)
                for f in fs
                if os.path.exists(os.path.join(dp, f))
            )
        else:
            size = os.path.getsize(artifact)
    return {
        "cmd": list(cmd),
        "build_succeeded": all(r["exit_code"] == 0 and not r["timed_out"] for r in runs),
        "compile_wall_seconds": _stats([r["wall_seconds"] for r in runs]),
        "representative_compile_seconds": (_stats([r["wall_seconds"] for r in runs]) or {}).get("median"),
        "artifact_path": artifact,
        "artifact_size_bytes": size,
        "runs": runs,
    }


def main():
    ap = argparse.ArgumentParser(description="Measure a command's time and memory.")
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--warmups", type=int, default=0)
    ap.add_argument("--cwd", default=None)
    ap.add_argument("--stdin", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--label", default=None)
    ap.add_argument("--out", default=None, help="write JSON here")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    if not a.cmd:
        ap.error("no command given")
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    res = measure(cmd, repeats=a.repeats, warmups=a.warmups, cwd=a.cwd,
                  stdin_path=a.stdin, timeout=a.timeout, label=a.label)
    text = json.dumps(res, indent=2)
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    else:
        print(text)


if __name__ == "__main__":
    main()
