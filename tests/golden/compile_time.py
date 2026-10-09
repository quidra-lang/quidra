#!/usr/bin/env python3
"""Gate Gperf: compile time of the compiler, per stage (README.md, Gates).

  compile_time.py --base-tool A --head-tool B --root CORPUS [--home DIR]
                  [--entry NAME ...] [--repeat 5] [--metric auto|instructions|wall]
                  [--limit 0.5]

For each input and each stage (lower, optimize, emit), runs
`quidra_golden_dump --time-stage STAGE --repeat N FILE` with the base and the
head tool, and reports the head's change in percent.

metric instructions (Linux): retired instructions of the stage function only,
  counted by callgrind with --toggle-collect (quidra::ir::lower,
  quidra::ir::optimize, quidra::emit_llvm), one repetition (the count is
  deterministic); the gate fails when a stage is more than --limit percent
  above the base (default 0.5).
metric wall (macOS, or without valgrind): the minimum of N wall-clock samples;
  reported, never gated (macOS reports wall time only).

Default inputs: an external training program and its layers module (corpus
externals, skipped when absent), the largest NN package program, the largest
Core example and the 10,000-statement generated file.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

STAGES = {"lower": "quidra::ir::lower*", "optimize": "quidra::ir::optimize*",
          "emit": "quidra::emit_llvm*"}
# The 40,000-statement file is slow to lower (the Lowerer is superlinear
# in the statement count), so the default uses the 10,000-statement file;
# pass --entry generated/flat_40000.qui to measure the larger one.
DEFAULT_ENTRIES = ["external/training-integ.qui", "external/layers.qui",
                   "store/nn/main.qui", "examples/fixed_arrays.qui",
                   "generated/flat_10000.qui"]


def entry_rows(root: Path) -> dict[str, list[str]]:
    rows = {}
    for line in (root / "entries.tsv").read_text(encoding="utf-8").splitlines():
        if line:
            fields = line.split("\t")
            rows[fields[0]] = fields
    return rows


def environment(home: Path, package_path: str) -> dict[str, str]:
    return {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home),
            "QUIDRA_PACKAGE_PATH": package_path, "LC_ALL": "C", "LANG": "C", "TZ": "UTC"}


def wall(tool: str, stage: str, repeat: int, row: list[str], home: Path) -> float:
    result = subprocess.run([tool, "--time-stage", stage, "--repeat", str(repeat), row[2]],
                            cwd=row[3], env=environment(home, row[4]), check=True,
                            stdout=subprocess.PIPE, text=True)
    return float(re.search(r" min (\d+)", result.stdout).group(1))


def instructions(tool: str, stage: str, repeat: int, row: list[str], home: Path) -> float:
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch) / "callgrind.out"
        subprocess.run(["valgrind", "--tool=callgrind", f"--callgrind-out-file={out}",
                        f"--toggle-collect={STAGES[stage]}", "--collect-atstart=no",
                        tool, "--time-stage", stage, "--repeat", str(repeat), row[2]],
                       cwd=row[3], env=environment(home, row[4]), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        text = out.read_text(encoding="utf-8", errors="replace")
        totals = re.search(r"^(?:summary|totals):\s+(\d+)", text, re.M)
        return float(totals.group(1)) / repeat


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-tool", required=True)
    parser.add_argument("--head-tool", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--home")
    parser.add_argument("--entry", action="append")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--metric", choices=["auto", "instructions", "wall"], default="auto")
    parser.add_argument("--limit", type=float, default=0.5)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    base_tool = str(Path(args.base_tool).resolve())
    head_tool = str(Path(args.head_tool).resolve())
    home = Path(args.home or root.parent / "home").resolve()
    home.mkdir(parents=True, exist_ok=True)
    metric = args.metric
    if metric == "auto":
        metric = "instructions" if sys.platform.startswith("linux") and shutil.which("valgrind") \
            else "wall"
    measure = instructions if metric == "instructions" else wall
    repeat = 1 if metric == "instructions" else args.repeat
    rows = entry_rows(root)
    failed = False
    print(f"compile_time.py: metric={metric} repeat={repeat}"
          + (f" limit=+{args.limit}%" if metric == "instructions" else " (report only)"))
    for name in args.entry or DEFAULT_ENTRIES:
        if name not in rows:
            print(f"  {name}: not in the corpus, skipped")
            continue
        for stage in STAGES:
            base = measure(base_tool, stage, repeat, rows[name], home)
            head = measure(head_tool, stage, repeat, rows[name], home)
            change = (head - base) / base * 100 if base else 0.0
            flag = ""
            if metric == "instructions" and change > args.limit:
                flag = "  FAIL"
                failed = True
            unit = "Ir" if metric == "instructions" else "ns"
            print(f"  {name:40} {stage:8} base {base:14.0f} {unit}  head {head:14.0f} {unit}"
                  f"  {change:+6.2f}%{flag}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
