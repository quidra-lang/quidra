#!/usr/bin/env python3
"""Asymptotic guards for runtime operations that were accidentally quadratic.

These assert *scaling*, not wall-clock times, so they stay meaningful on a slow
or loaded machine: doubling the input may roughly double the work, but it must
not roughly quadruple it. Each regression they cover was a per-operation scan of
a structure that grows with the loop, which is invisible in a small test and
turns an ordinary loop into an O(n^2) one.
"""
import os
import statistics
import subprocess
import sys
import tempfile
import time

QUIDRA = sys.argv[1]
ROOT = sys.argv[2] if len(sys.argv) > 2 else "."

# A sanitizer build instruments every allocation and access, so its timings say
# nothing about the asymptotics of the uninstrumented runtime. Correctness is
# covered by the other suites, which do run there.
if os.environ.get("ASAN_OPTIONS") or os.environ.get("UBSAN_OPTIONS"):
    print("skipped: sanitizer build")
    raise SystemExit(0)

# A quadratic implementation costs ~4x when the input doubles and a linear one
# ~2x. Keep the 3x boundary: measurement quality must improve rather than making
# the regression threshold less meaningful.
MAX_DOUBLING_RATIO = 3.0
RUNS = 7
WARMUP_RUNS = 2
MAX_STARTUP_FRACTION = 0.25
MAX_WORKLOAD_GROWTHS = 4


def median_seconds(program, work_dir):
    binary = os.path.join(work_dir, "prog")
    source = os.path.join(work_dir, "prog.qui")
    with open(source, "w") as f:
        f.write(program)
    subprocess.run([QUIDRA, "build", source, "-o", binary], check=True,
                   stdout=subprocess.DEVNULL)

    def run_once():
        start = time.perf_counter()
        result = subprocess.run([binary], capture_output=True, text=True,
                                cwd=work_dir)
        elapsed = time.perf_counter() - start
        if result.returncode != 0:
            raise SystemExit(f"program failed: {result.stderr}")
        return elapsed, result.stdout.strip()

    for _ in range(WARMUP_RUNS):
        run_once()

    samples = []
    output = None
    for _ in range(RUNS):
        elapsed, current_output = run_once()
        if output is not None and current_output != output:
            raise SystemExit("program output changed between timing samples")
        output = current_output
        samples.append(elapsed)
    return statistics.median(samples), output


# Walking a second array must not cost dramatically more per element than
# walking one; the regression this catches made it over five times more.
MAX_SECOND_ARRAY_RATIO = 2.0


def check_ratio(name, baseline, candidate, baseline_out, candidate_out):
    with tempfile.TemporaryDirectory() as work_dir:
        base_s, got_base = median_seconds(baseline, work_dir)
        cand_s, got_cand = median_seconds(candidate, work_dir)
    for got, want in ((got_base, baseline_out), (got_cand, candidate_out)):
        if got != want:
            raise SystemExit(f"{name}: expected {want!r}, got {got!r}")
    ratio = cand_s / base_s
    if ratio > MAX_SECOND_ARRAY_RATIO:
        raise SystemExit(
            f"{name}: touching two arrays cost {ratio:.1f}x what touching one did "
            f"({base_s * 1000:.1f} ms -> {cand_s * 1000:.1f} ms); the per-access "
            f"initialization lookup is no longer resolved from cache")
    print(f"  {name}: {base_s * 1000:8.1f} ms -> {cand_s * 1000:8.1f} ms "
          f"({ratio:.2f}x for the second array)")


def check_scaling(name, template, small, large, expect):
    with tempfile.TemporaryDirectory() as work_dir:
        startup_s, startup_out = median_seconds(template.format(n=0), work_dir)
        if startup_out != expect(0):
            raise SystemExit(f"{name}: expected {expect(0)!r}, got {startup_out!r}")

        measured = None
        for _ in range(MAX_WORKLOAD_GROWTHS + 1):
            small_s, small_out = median_seconds(template.format(n=small), work_dir)
            large_s, large_out = median_seconds(template.format(n=large), work_dir)
            for got, want in ((small_out, expect(small)), (large_out, expect(large))):
                if got != want:
                    raise SystemExit(f"{name}: expected {want!r}, got {got!r}")

            # Subtract a separately measured zero-work process cost instead of
            # inventing a floor from the values under test. If startup is still
            # too large a fraction of the smaller sample, grow both workloads
            # together until the asymptotic signal dominates scheduler/process
            # noise while preserving the exact 2x input comparison.
            if startup_s < small_s and startup_s / small_s <= MAX_STARTUP_FRACTION:
                measured = (small, large, small_s, large_s)
                break
            small *= 2
            large *= 2

    if measured is None:
        raise SystemExit(
            f"{name}: process startup ({startup_s * 1000:.1f} ms) remained too large "
            "relative to the workload to measure asymptotic scaling reliably")

    small, large, small_s, large_s = measured
    small_work = small_s - startup_s
    large_work = large_s - startup_s
    ratio = large_work / small_work
    if ratio > MAX_DOUBLING_RATIO:
        raise SystemExit(
            f"{name}: doubling the input multiplied measured work by {ratio:.1f}x "
            f"after subtracting the independently measured {startup_s * 1000:.1f} ms startup "
            f"({small_s * 1000:.1f} ms at n={small} -> {large_s * 1000:.1f} ms at n={large}); "
            f"the operation is no longer linear")
    print(
        f"  {name}: startup {startup_s * 1000:6.1f} ms; "
        f"{small_s * 1000:8.1f} ms at n={small} -> {large_s * 1000:8.1f} ms at n={large} "
        f"({ratio:.2f}x work for 2x input)")


# Appending to a string re-read the whole accumulated prefix on every append,
# both to measure it and to re-validate it as UTF-8, so building text in a loop
# was quadratic even though the backing capacity already grew geometrically.
check_scaling(
    "string append in a loop",
    """int n = {n}
string acc = ""
for i in range(0, n)
    acc = acc + "abcdefghij"
print(len(acc))
""",
    40000, 80000, lambda n: str(n * 10))

# Every checked element access through a writable array reference consults the
# runtime initialization tracker, which cached exactly one allocation. A loop
# that reads one array while writing another missed that cache on every access
# and fell back to a tree lookup, so touching two arrays cost several times what
# touching one did. The two programs below differ only in how many arrays they
# walk, so the ratio between them isolates that cost from machine speed.
check_ratio(
    "two-array vs one-array access through &",
    """void scale(int[] &v, int n)
    for i in range(0, n)
        v[i] = v[i] + 1

int n = 400000
int[] v = array(n, fill = 0)
for pass in range(0, 8)
    scale(&v, n)
print(v[0])
""",
    """void copy_into(int[] &destination, int[] &source, int n)
    for i in range(0, n)
        destination[i] = source[i] + 1

int n = 400000
int[] source = array(n, fill = 0)
int[] destination = array(n, fill = 0)
for pass in range(0, 8)
    copy_into(&destination, &source, n)
print(destination[0])
""",
    "8", "1")

print("performance regressions passed")