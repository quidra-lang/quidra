#!/usr/bin/env python3
"""Asymptotic guards for runtime operations that were accidentally quadratic.

These assert *scaling*, not wall-clock times, so they stay meaningful on a slow
or loaded machine: doubling the input may roughly double the work, but it must
not roughly quadruple it. Each regression they cover was a per-operation scan of
a structure that grows with the loop, which is invisible in a small test and
turns an ordinary loop into an O(n^2) one.
"""
import os
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
# ~2x. The observed regressions were far worse than 4x, so this leaves a wide
# margin for scheduling noise while still failing on a genuine reintroduction.
MAX_DOUBLING_RATIO = 3.0
RUNS = 3


def best_seconds(program, work_dir):
    binary = os.path.join(work_dir, "prog")
    source = os.path.join(work_dir, "prog.qui")
    with open(source, "w") as f:
        f.write(program)
    subprocess.run([QUIDRA, "build", source, "-o", binary], check=True,
                   stdout=subprocess.DEVNULL)
    best = None
    for _ in range(RUNS):
        start = time.perf_counter()
        result = subprocess.run([binary], capture_output=True, text=True,
                                cwd=work_dir)
        elapsed = time.perf_counter() - start
        if result.returncode != 0:
            raise SystemExit(f"program failed: {result.stderr}")
        best = elapsed if best is None else min(best, elapsed)
    return best, result.stdout.strip()


# Walking a second array must not cost dramatically more per element than
# walking one; the regression this catches made it over five times more.
MAX_SECOND_ARRAY_RATIO = 2.0


def check_ratio(name, baseline, candidate, baseline_out, candidate_out):
    with tempfile.TemporaryDirectory() as work_dir:
        base_s, got_base = best_seconds(baseline, work_dir)
        cand_s, got_cand = best_seconds(candidate, work_dir)
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
        small_s, small_out = best_seconds(template.format(n=small), work_dir)
        large_s, large_out = best_seconds(template.format(n=large), work_dir)
    for got, want in ((small_out, expect(small)), (large_out, expect(large))):
        if got != want:
            raise SystemExit(f"{name}: expected {want!r}, got {got!r}")
    # Process startup dominates a fast run, so compare the work above a floor
    # rather than the raw wall times.
    floor = min(small_s, large_s) * 0.5
    ratio = max(large_s - floor, 1e-9) / max(small_s - floor, 1e-9)
    if ratio > MAX_DOUBLING_RATIO:
        raise SystemExit(
            f"{name}: doubling the input multiplied the work by {ratio:.1f}x "
            f"({small_s * 1000:.1f} ms -> {large_s * 1000:.1f} ms); "
            f"the operation is no longer linear")
    print(f"  {name}: {small_s * 1000:8.1f} ms -> {large_s * 1000:8.1f} ms "
          f"({ratio:.2f}x for 2x input)")


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
