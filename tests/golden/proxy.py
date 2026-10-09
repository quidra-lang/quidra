#!/usr/bin/env python3
"""Performance proxies without timing: code size per function (README.md, Inputs).

  proxy.py report --capture DIR [--only PREFIX]... [--clang CLANG] [--opt OPT]
                  [--triple TRIPLE] [--keep DIR] [-j N] [--out FILE]
  proxy.py compare BASE HEAD [--only PREFIX]... [--cold]

`report` reads the `llvm` view of every entry of a capture (DIR/out/ENTRY/llvm)
and writes one line per function:

  function <entry> <function> llvm=<hot>/<cold> [asm=<hot>/<cold>] [vectorized=<n>]

- llvm: the instructions of the function in the emitted LLVM IR, those of cold
  blocks apart (a block whose terminator is `unreachable`, or that only stores
  and branches on: the K3 blocks of expect.py);
- asm (with --clang): the machine instructions of `CLANG -O3 -S` of the
  entry's LLVM IR, those of blocks that come from cold IR blocks apart (the
  assembler's block comments name the IR block), and fragments that the
  compiler split off (`F.cold.N`) counted as cold code of F;
- vectorized (with --opt): the loops `OPT -O3` vectorized in the function
  (its loop-vectorize remarks). The emitted IR names no target, so OPT gets
  --triple (by default the one CLANG targets), whose cost model decides.

--keep DIR also writes, per entry, the optimized IR (`CLANG -O3 -S
-emit-llvm`, function by function) and the assembly, for structural checks.

`compare` reads two reports (base, head) and lists every function whose hot
instruction count (llvm and asm; with --cold also cold) rose, or whose
vectorized loops fell; it exits 1 when there is one. Functions on one side
only are listed apart and do not fail.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from expect import DEFINE, contexts  # noqa: E402

LABEL_LINE = re.compile(r"^[A-Za-z$._0-9-]+:")
ASM_FUNCTION = re.compile(r"^(_?[A-Za-z$._][A-Za-z0-9$._]*|\"[^\"]+\"):")
ASM_BLOCK = re.compile(r"^(L|\.L)BB\d+_\d+:")
ASM_BLOCK_NAME = re.compile(r"(?:##|#|//|;)\s*%(\S+)\s*$")
COLD_FRAGMENT = re.compile(r"^(.*)\.cold(?:\.\d+)?$")


def llvm_counts(text: str) -> tuple[dict[str, list[int]], dict[str, set[str]]]:
    """Function -> [hot, cold] instruction counts, and function -> the labels
    of its cold blocks."""
    lines = text.splitlines()
    result: dict[str, list[int]] = {}
    cold_labels: dict[str, set[str]] = defaultdict(set)
    for line, context in zip(lines, contexts(lines)):
        if context.function is None or DEFINE.match(line):
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith(";") or stripped == "}" or LABEL_LINE.match(stripped):
            continue
        counts = result.setdefault(context.function, [0, 0])
        counts[1 if context.cold else 0] += 1
        if context.cold and context.label:
            cold_labels[context.function].add(context.label)
    return result, cold_labels


def asm_symbol(name: str) -> str:
    name = name.strip('"')
    return name[1:] if name.startswith("_") else name


def asm_counts(text: str, cold_labels: dict[str, set[str]]) -> dict[str, list[int]]:
    """Function -> [hot, cold] machine instructions of `clang -O3 -S` output."""
    result: dict[str, list[int]] = {}
    function = None
    cold = False
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        if not line[0].isspace():
            block = ASM_BLOCK.match(line)
            if block and function:
                name = ASM_BLOCK_NAME.search(line)
                base = COLD_FRAGMENT.match(function)
                owner = base.group(1) if base else function
                cold = bool(base) or bool(name and name.group(1) in cold_labels.get(owner, set()))
                continue
            symbol = ASM_FUNCTION.match(line)
            if symbol and not line.startswith(("L", ".L", "l_", "ltmp", ".Ltmp", "Ltmp")):
                function = asm_symbol(symbol.group(1))
                cold = bool(COLD_FRAGMENT.match(function))
            continue
        stripped = line.strip()
        if function is None or stripped.startswith((".", "#", ";", "//", "@")):
            continue
        base = COLD_FRAGMENT.match(function)
        owner = base.group(1) if base else function
        counts = result.setdefault(owner, [0, 0])
        counts[1 if cold else 0] += 1
    return result


def vectorized_loops(remarks: str) -> dict[str, int]:
    """Function -> loops vectorized, from an `opt -pass-remarks-output` file."""
    result: dict[str, int] = defaultdict(int)
    for document in remarks.split("--- !")[1:]:
        fields = dict(re.findall(r"^(Pass|Name|Function):\s*(.+?)\s*$", document, re.M))
        if document.startswith("Passed") and fields.get("Pass") == "loop-vectorize" and \
                fields.get("Name") == "Vectorized":
            result[fields.get("Function", "?").strip("'\"")] += 1
    return result


def measure(entry: str, path: Path, args) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    llvm, cold_labels = llvm_counts(text)
    asm: dict[str, list[int]] = {}
    vectorized: dict[str, int] = {}
    if args.clang or args.opt:
        with tempfile.TemporaryDirectory() as scratch:
            module = Path(scratch) / "entry.ll"
            module.write_text(text, encoding="utf-8")
            if args.clang:
                assembly = Path(scratch) / "entry.s"
                subprocess.run([args.clang, "-O3", "-S", "-x", "ir", str(module), "-o", str(assembly)],
                               check=True, stderr=subprocess.DEVNULL)
                asm = asm_counts(assembly.read_text(encoding="utf-8", errors="replace"), cold_labels)
                if args.keep:
                    keep = Path(args.keep) / entry
                    keep.parent.mkdir(parents=True, exist_ok=True)
                    keep.with_name(keep.name + ".s").write_text(assembly.read_text(encoding="utf-8"),
                                                                encoding="utf-8")
                    subprocess.run([args.clang, "-O3", "-S", "-emit-llvm", "-x", "ir", str(module),
                                    "-o", str(keep.with_name(keep.name + ".O3.ll"))],
                                   check=True, stderr=subprocess.DEVNULL)
            if args.opt:
                remarks = Path(scratch) / "remarks.yaml"
                triple = [f"-mtriple={args.triple}"] if args.triple else []
                subprocess.run([args.opt, "-O3", *triple, "-disable-output",
                                "-pass-remarks=loop-vectorize",
                                f"-pass-remarks-output={remarks}", str(module)],
                               check=True, stderr=subprocess.DEVNULL)
                vectorized = vectorized_loops(remarks.read_text(encoding="utf-8", errors="replace")
                                              if remarks.exists() else "")
    lines = []
    for function in sorted(set(llvm) | set(asm)):
        hot, cold = llvm.get(function, [0, 0])
        line = f"function {entry} {function} llvm={hot}/{cold}"
        if args.clang:
            asm_hot, asm_cold = asm.get(function, [0, 0])
            line += f" asm={asm_hot}/{asm_cold}"
        if args.opt:
            line += f" vectorized={vectorized.get(function, 0)}"
        lines.append(line)
    return lines


def entries(capture: Path, only: list[str]) -> list[tuple[str, Path]]:
    found = []
    index = capture / "index.tsv"
    if index.exists():
        for row in index.read_text(encoding="utf-8").splitlines():
            parts = row.split("\t")
            if len(parts) >= 5 and parts[1] == "llvm" and parts[4] == "ok":
                found.append(parts[0])
    else:
        found = sorted(str(p.parent.relative_to(capture / "out")) for p in (capture / "out").rglob("llvm"))
    selected = [e for e in found if not only or any(e.startswith(prefix) for prefix in only)]
    return [(e, capture / "out" / e / "llvm") for e in sorted(selected)]


def report(args) -> None:
    if args.opt and not args.triple and args.clang:
        args.triple = subprocess.run([args.clang, "-print-target-triple"], check=True,
                                     stdout=subprocess.PIPE, text=True).stdout.strip()
    work = entries(Path(args.capture), args.only)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda item: measure(item[0], item[1], args), work))
    text = "".join(line + "\n" for lines in results for line in lines)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


def read_report(path: str, only: list[str]) -> dict[tuple[str, str], dict[str, list[int]]]:
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split(" ")
        if len(parts) < 4 or parts[0] != "function":
            continue
        if only and not any(parts[1].startswith(prefix) for prefix in only):
            continue
        values = {}
        for field in parts[3:]:
            name, _, value = field.partition("=")
            values[name] = [int(v) for v in value.split("/")]
        result[(parts[1], parts[2])] = values
    return result


def compare(args) -> None:
    base = read_report(args.base, args.only)
    head = read_report(args.head, args.only)
    failures = []
    for key in sorted(set(base) & set(head)):
        before, after = base[key], head[key]
        for measure_name in ("llvm", "asm"):
            if measure_name in before and measure_name in after:
                parts = (0, 1) if args.cold else (0,)
                for part in parts:
                    if after[measure_name][part] > before[measure_name][part]:
                        kind = "hot" if part == 0 else "cold"
                        failures.append(f"{key[0]} {key[1]}: {measure_name} {kind} "
                                        f"{before[measure_name][part]} -> {after[measure_name][part]}")
        if "vectorized" in before and "vectorized" in after and \
                after["vectorized"][0] < before["vectorized"][0]:
            failures.append(f"{key[0]} {key[1]}: vectorized loops "
                            f"{before['vectorized'][0]} -> {after['vectorized'][0]}")
    for key in sorted(set(base) - set(head)):
        print(f"only in base: {key[0]} {key[1]}")
    for key in sorted(set(head) - set(base)):
        print(f"only in head: {key[0]} {key[1]}")
    for failure in failures:
        print(f"rose: {failure}")
    print(f"proxy.py: {len(set(base) & set(head))} functions compared, {len(failures)} above the base")
    raise SystemExit(1 if failures else 0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    r = commands.add_parser("report")
    r.add_argument("--capture", required=True)
    r.add_argument("--only", action="append", default=[])
    r.add_argument("--clang")
    r.add_argument("--opt")
    r.add_argument("--triple")
    r.add_argument("--keep")
    r.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    r.add_argument("--out")
    c = commands.add_parser("compare")
    c.add_argument("base")
    c.add_argument("head")
    c.add_argument("--only", action="append", default=[])
    c.add_argument("--cold", action="store_true")
    args = parser.parse_args()
    {"report": report, "compare": compare}[args.command](args)


if __name__ == "__main__":
    main()
