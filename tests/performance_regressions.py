#!/usr/bin/env python3
"""Asymptotic guards for runtime operations that were accidentally quadratic.

These assert *scaling*, not wall-clock times, so they stay meaningful on a slow
or loaded machine: doubling the input may roughly double the work, but it must
not roughly quadruple it. Each regression they cover was a per-operation scan of
a structure that grows with the loop, which is invisible in a small test and
turns an ordinary loop into an O(n^2) one.
"""
import json
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
MEASUREMENTS = []


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


def paired_median_seconds(first_program, second_program, work_dir):
    """Measure two workloads in alternating order to cancel runner load drift."""
    def build(program, stem):
        source = os.path.join(work_dir, f"{stem}.qui")
        binary = os.path.join(work_dir, stem)
        with open(source, "w") as f:
            f.write(program)
        subprocess.run(
            [QUIDRA, "build", source, "-o", binary],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        return binary

    def run_once(binary):
        start = time.perf_counter()
        result = subprocess.run(
            [binary], capture_output=True, text=True, cwd=work_dir
        )
        elapsed = time.perf_counter() - start
        if result.returncode != 0:
            raise SystemExit(f"program failed: {result.stderr}")
        return elapsed, result.stdout.strip()

    first_binary = build(first_program, "first")
    second_binary = build(second_program, "second")

    for index in range(WARMUP_RUNS):
        order = (first_binary, second_binary)
        if index % 2:
            order = tuple(reversed(order))
        for binary in order:
            run_once(binary)

    first_samples = []
    second_samples = []
    first_output = None
    second_output = None
    for index in range(RUNS):
        order = (
            ((first_binary, first_samples, "first"),
             (second_binary, second_samples, "second"))
            if index % 2 == 0
            else
            ((second_binary, second_samples, "second"),
             (first_binary, first_samples, "first"))
        )
        for binary, samples, which in order:
            elapsed, output = run_once(binary)
            if which == "first":
                if first_output is not None and output != first_output:
                    raise SystemExit(
                        "first program output changed between timing samples")
                first_output = output
            else:
                if second_output is not None and output != second_output:
                    raise SystemExit(
                        "second program output changed between timing samples")
                second_output = output
            samples.append(elapsed)

    return (
        statistics.median(first_samples),
        first_output,
        statistics.median(second_samples),
        second_output,
    )


# Walking a second array must not cost dramatically more per element than
# walking one; the regression this catches made it over five times more.
MAX_SECOND_ARRAY_RATIO = 2.5


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
    MEASUREMENTS.append({
        "name": name,
        "kind": "ratio",
        "baseline_seconds": base_s,
        "candidate_seconds": cand_s,
        "ratio": ratio,
    })
    print(f"  {name}: {base_s * 1000:8.1f} ms -> {cand_s * 1000:8.1f} ms "
          f"({ratio:.2f}x for the second array)")


def check_correctness_regression(name, program, expected):
    with tempfile.TemporaryDirectory() as work_dir:
        source = os.path.join(work_dir, "regression.qui")
        with open(source, "w", encoding="utf-8") as output:
            output.write(program)
        try:
            result = subprocess.run(
                [QUIDRA, "run", source],
                cwd=work_dir,
                capture_output=True,
                text=True,
                timeout=30,
            )
        except subprocess.TimeoutExpired as exc:
            raise SystemExit(f"{name}: timed out; possible compiler/runtime regression") from exc
    if result.returncode != 0:
        raise SystemExit(f"{name}: program failed: {result.stderr}")
    got = result.stdout.strip()
    if got != expected:
        raise SystemExit(f"{name}: expected {expected!r}, got {got!r}")
    MEASUREMENTS.append({"name": name, "kind": "correctness", "status": "passed"})
    print(f"  {name}: passed")


def check_scaling(name, template, small, large, expect, allow_too_fast=False):
    with tempfile.TemporaryDirectory() as work_dir:
        startup_s, startup_out = median_seconds(template.format(n=0), work_dir)
        if startup_out != expect(0):
            raise SystemExit(f"{name}: expected {expect(0)!r}, got {startup_out!r}")

        measured = None
        for _ in range(MAX_WORKLOAD_GROWTHS + 1):
            small_s, small_out, large_s, large_out = paired_median_seconds(
                template.format(n=small),
                template.format(n=large),
                work_dir,
            )
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
        if allow_too_fast:
            MEASUREMENTS.append({
                "name": name,
                "kind": "scaling",
                "status": "below_startup_resolution",
                "startup_seconds": startup_s,
            })
            print(
                f"  {name}: workload stayed below the startup-resolution threshold "
                f"({startup_s * 1000:.1f} ms startup); fast path accepted")
            return
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
    MEASUREMENTS.append({
        "name": name,
        "kind": "scaling",
        "status": "measured",
        "startup_seconds": startup_s,
        "small_n": small,
        "large_n": large,
        "small_seconds": small_s,
        "large_seconds": large_s,
        "work_ratio": ratio,
    })
    print(
        f"  {name}: startup {startup_s * 1000:6.1f} ms; "
        f"{small_s * 1000:8.1f} ms at n={small} -> {large_s * 1000:8.1f} ms at n={large} "
        f"({ratio:.2f}x work for 2x input)")


# MB08 once placed loop-local string storage in the LLVM loop body. Nested
# loops plus repeated concatenation then grew the native stack until the
# benchmark crashed. Preserve that structural trigger separately from the
# scaling assertion below.
check_correctness_regression(
    "MB08 loop-local string stack regression",
    """int total = 0
for outer in range(0, 2048)
    string row = ""
    for inner in range(0, 512)
        row = row + "x"
    total += len(row)
print(total)
""",
    "1048576",
)

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

# Repeated prefix operations must not revalidate or index every byte of the
# growing source on each iteration. These two regressions used to turn otherwise
# constant-prefix work into O(n^2).
check_scaling(
    "string starts_with reuses validation metadata",
    """int n = {n}
string source = "x"
for i in range(0, n)
    source = source + "x"
int matches = 0
for i in range(0, n)
    if source.starts_with("x")
        matches += 1
print(matches)
""",
    5000, 10000, lambda n: str(n), allow_too_fast=True)

check_scaling(
    "short string slice does not scan the full source",
    """int n = {n}
string source = "x"
for i in range(0, n)
    source = source + "x"
int total = 0
for i in range(0, n)
    total += len(source.slice(0, 1))
print(total)
""",
    3000, 6000, lambda n: str(n), allow_too_fast=True)

# Explicit bin -> UTF-8 decoding validates and copies each input once. Repeating
# that conversion must stay linear in the byte count rather than materializing
# per-byte text fragments or rescanning a growing prefix.
check_scaling(
    "explicit UTF-8 bin decoding",
    """int n = {n}
string source = string.repeat("a", n)
bin data = source.utf8()
int total = 0
for pass in range(0, 8)
    match string.from_utf8(data)
        string text
            total += len(text)
        error problem
            process.exit(1)
print(total)
""",
    250000, 500000, lambda n: str(n * 8), allow_too_fast=True)

# Standard map deletion uses O(1) tombstones, bounded slot rehashing, and
# occasional stable-order compaction. Building, deleting half the entries, and
# enumerating survivors must remain roughly linear rather than turning repeated
# deletion or compaction into quadratic work.
check_scaling(
    "map removal and compaction",
    """int n = {n}
map.Map<int, int> values = map.Map<int, int>()
for i in range(0, n)
    values.set(i, i)
for i in range(0, n, 2)
    values.remove(i)
int[] remaining = values.keys()
print(len(remaining))
""",
    12000, 24000, lambda n: str(n // 2), allow_too_fast=True)

# Fully initialized array references cache their initialization state once at
# function entry when the referenced binding cannot be replaced or escaped.
# The remaining per-element branch should stay a small constant overhead versus
# the statically proven direct-array path.
check_ratio(
    "fully initialized array reference fast path",
    """int n = 400000
int[] values = array(n, fill = 1)
int total = 0
for pass in range(0, 8)
    for i in range(0, n)
        total += values[i]
print(total)
""",
    """int sum_all(const int[] &values, int n)
    int total = 0
    for pass in range(0, 8)
        for i in range(0, n)
            total += values[i]
    return total

int n = 400000
int[] values = array(n, fill = 1)
print(sum_all(&values, n))
""",
    "3200000", "3200000")

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

output_path = os.environ.get("QUIDRA_PERF_JSON")
if output_path:
    with open(output_path, "w", encoding="utf-8") as output:
        json.dump({
            "kind": "quidra-runtime-performance-regressions",
            "measurements": MEASUREMENTS,
        }, output, indent=2)
        output.write("\n")

print("performance regressions passed")