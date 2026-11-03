#!/usr/bin/env python3
"""Self-test of the census keys and counts (census.hpp, census_keys.hpp).

  census_tests.py --tool quidra_golden_dump

Captures the `census` view of small programs with the golden tool and checks
the listed sites of every key and a few counts: each program lists exactly
the sites its comments name, and its unlisted twin lists none.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

PROGRAMS: dict[str, str] = {
    # compound-store-across-call: an element and a field target whose
    # right-hand side calls; the plain form and a call-free compound are
    # not listed.
    "compound.qui": """class Box this(local int n_initial)
    int n

    this.n = n_initial

int grow(int[] &values)
    values = values.append(1)
    return 1

int bump(Box &box)
    box.n = box.n + 1
    return 1

int[] a = [1, 2]
a[0] += grow(&a)
a[1] = a[1] + grow(&a)
a[0] += 1
Box b = Box(1)
b.n += bump(&b)
print("{a[0]} {b.n}")
print(NL)
""",
    # collection-loop-writes-iterable: the first loop appends to the array
    # it iterates, the second only reads it.
    "loops.qui": """\
int[] values = [1, 2, 3]
for auto v = values[]
    values = values.append(v)
int total = 0
for auto v = values[]
    total = total + v
print("{total}")
print(NL)
""",
    # assign-order: tier 1 (the right-hand side writes the index), tier 2
    # (a failing index read on the left, a call on the right; checked
    # arithmetic on the left, a failing read on the right); a variable
    # index and a call-free value are not listed.
    "order.qui": """\
int bump(int &i)
    i = i + 1
    return 7

int seven()
    return 7

int[] a = [0, 0, 0, 0]
int[] b = [1, 2]
int i = 0
int j = 0
a[i] = bump(&i)
a[b[j]] = seven()
a[i + 1] = b[j]
a[i] = seven()
a[j] = 5
print("{a[0]} {a[1]} {a[2]}")
print(NL)
""",
    # reference-loop-alias-call: a reference loop over a `&` parameter whose
    # body assigns another reference that can hold the array (one listed
    # statement); the body also reaches the elements through that
    # reference (reference-loop-current-element-alias). The loop over a
    # local array is neither.
    "references.qui": """\
void walk(int[] &a, int[] &b)
    for auto &x = &a[]
        b = b.append(9)
        x = x + 100

void plain()
    int[] local_values = [1, 2]
    for auto &x = &local_values[]
        x = x + 1

int[] v = [1, 2, 3]
int[] w = [4]
walk(&v, &w)
plain()
print("{v[0]} {len(w)}")
print(NL)
""",
    # typed-constant-retyped: the uses of an imported real32 constant at
    # another type. A typed constant keeps its declared type, so such a
    # use is a compile error and an explicit conversion is not listed.
    "constants/lib.qui": """\
const real32 eps = 0.5
const real32 tiny = 0.25
""",
    "constants/main.qui": """\
import lib = "./lib.qui"

real64 wide = real64(lib.eps)
real32 narrow = lib.tiny
print("{wide} {narrow}")
print(NL)
""",
    # argument-isolation-copy: a borrowed argument whose storage the same
    # call grows through a `&` argument; the call with other storage is not
    # listed.
    "isolation.qui": """\
int first_after(int[] values, int[] &target)
    target = target.append(1)
    return values[0]

int[] a = [5]
int[] b = [7]
print("{first_after(a, &a)}")
print("{first_after(b, &a)}")
print(NL)
""",
    # Only the copy used as a gradient destination is listed; touching the
    # original's gradient state does not demand every other copy in the scope.
    "target-copy.qui": """\
autograd.Target source = autograd.target()
autograd.Target used = source
autograd.Target ignored = source
print(used.has_grad())
print(source.has_grad())
print(NL)
""",
    "target-flow.qui": """\
autograd.Target echo(autograd.Target value)
    return value
autograd.Target source = autograd.target()
autograd.Target copy = echo(source)
print(copy.has_grad())
print(NL)
""",
    "target-loop.qui": """\
autograd.Target source = autograd.target()
autograd.Target[] values = [source]
for auto value = values[]
    print(value.has_grad())
print(NL)
""",
    "target-pattern.qui": """\
(autograd.Target, int) pair = (autograd.target(), 0)
(autograd.Target used, int count) = pair
print(used.has_grad())
print(NL)
""",
    "target-pattern-reference.qui": """\
(autograd.Target, int) pair = (autograd.target(), 0)
(autograd.Target &used, int &count) = &pair
print(used.has_grad())
print(NL)
""",
    "target-pattern-place.qui": """\
(autograd.Target, int) pair = (autograd.target(), 0)
autograd.Target used = autograd.target()
int count = 0
(used, count) = pair
print(used.has_grad())
print(NL)
""",
    # Checks and loop operations.
    "counts.qui": """\
int total = 0
for 0 <= nat i < 10
    total = total + int(i)
int8 small = 3
int64 k = 0
while k < 5
    k = k + 1
print("{total} {small} {k}")
print(NL)
""",
}

EXPECTED: dict[str, dict[str, int]] = {
    "compound.qui": {"key.compound-store-across-call": 2, "key.assign-order": 0},
    "loops.qui": {"key.collection-loop-writes-iterable": 1},
    "order.qui": {"key.assign-order": 3, "key.assign-order.tier1": 1, "key.assign-order.tier2": 2,
                  "key.compound-store-across-call": 0},
    "references.qui": {"key.reference-loop-alias-call": 1,
                       "key.reference-loop-alias-call.statements": 1,
                       "key.reference-loop-current-element-alias": 1},
    "constants/main.qui": {"key.typed-constant-retyped": 0, "kind.real64": 2, "kind.real32": 3},
    "isolation.qui": {"key.argument-isolation-copy": 1},
    "target-copy.qui": {"key.autograd-target-copy": 1},
    "target-flow.qui": {"key.autograd-target-copy": 2},
    "target-loop.qui": {"key.autograd-target-copy": 2},
    "target-pattern.qui": {"key.autograd-target-copy": 1},
    "target-pattern-reference.qui": {"key.autograd-target-copy": 0},
    "target-pattern-place.qui": {"key.autograd-target-copy": 1},
    "counts.qui": {"count.loop.overflow-checked-step": 1, "key.assign-order": 0,
                   "key.collection-loop-writes-iterable": 0},
}

# Sites by line: entry -> key -> the lines its sites must have.
SITE_LINES: dict[str, dict[str, list[int]]] = {
    "compound.qui": {"compound-store-across-call": [15, 19]},
    "order.qui": {"assign-order.tier1": [12], "assign-order.tier2": [13, 14]},
    "references.qui": {"reference-loop-alias-call": [2],
                       "reference-loop-alias-call.statements": [3]},
    "isolation.qui": {"argument-isolation-copy": [7]},
    "target-copy.qui": {"autograd-target-copy": [2]},
    "target-flow.qui": {"autograd-target-copy": [2, 4]},
    "target-loop.qui": {"autograd-target-copy": [2, 3]},
}


def census_values(text: str) -> dict[str, int]:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from expect import census_values as parse  # noqa: E402
    return parse(text)


def mixed_aggregation() -> None:
    """Keep alternative counts separate from named counts across components."""
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch)
        entries = ("examples/a.qui", "probes/b.qui")
        (root / "entries.tsv").write_text(
            "".join(f"{name}\tfile\tunused\tunused\tunused\n" for name in entries),
            encoding="utf-8")
        views = {
            (entries[0], "census"): "alt 0:SourceLocation 1 1\nalt 1:ConstantInt 2 1\n"
            "count loop.steps 4 3\nkey known 1\nkind int64 2\n",
            (entries[1], "census"): "alt 0:SourceLocation 3 2\nalt 1:ConstantInt 5 4\n"
            "count loop.steps 6 5\nkey known 2\nkind int64 3\n",
            (entries[1], "census.repl"): "alt 0:SourceLocation 2 1\nalt 1:ConstantInt 1 1\n"
            "count loop.steps 7 6\nkey known 1\nkind int64 4\n",
        }
        for (entry, view), text in views.items():
            path = root / "out" / entry / view
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).with_name("corpus.py")), "census",
             "--captures", str(root / "out"), "--root", str(root), "--require-all"],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"mixed census aggregation failed:\n{result.stderr}")
        for row in ("0:SourceLocation\t1/1\t5/3\t6/4",
                    "1:ConstantInt\t2/1\t6/5\t8/6",
                    "count\tcore-examples\tprobes\ttotal",
                    "loop.steps\t4/3\t13/11\t17/14",
                    "known\t1\t3\t4", "int64\t2\t7\t9"):
            if row not in result.stdout.splitlines():
                raise AssertionError(f"missing aggregate {row!r}:\n{result.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True)
    args = parser.parse_args()
    mixed_aggregation()
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch) / "programs"
        for name, text in PROGRAMS.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        for entry, expected in EXPECTED.items():
            out = Path(scratch) / "out" / entry
            source = root / entry
            subprocess.run([args.tool, "--root", str(source.parent), "--out", str(out),
                            "--cwd", str(source.parent), "--views", "census", source.name],
                           check=True)
            census = out / "census"
            if not census.exists():
                failures.append(f"{entry}: no census view ({sorted(p.name for p in out.iterdir())})")
                continue
            text = census.read_text(encoding="utf-8")
            values = census_values(text)
            for name, count in expected.items():
                if values.get(name, 0) != count:
                    failures.append(f"{entry}: {name} = {values.get(name, 0)}, expected {count}")
            for key, lines in SITE_LINES.get(entry, {}).items():
                found = sorted(int(row.split(" ")[2].rsplit(":", 2)[1])
                               for row in text.splitlines()
                               if row.startswith(f"site {key} "))
                if found != lines:
                    failures.append(f"{entry}: sites of {key} on lines {found}, expected {lines}")
            for row in text.splitlines():
                if row.startswith("key ") and not row.split(" ")[1]:
                    failures.append(f"{entry}: unnamed key row {row!r}")
    for failure in failures:
        print(f"census_tests.py: FAIL {failure}")
    if failures:
        raise SystemExit(1)
    print(f"census_tests.py: {len(EXPECTED)} programs passed")


if __name__ == "__main__":
    main()
