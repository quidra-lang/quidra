#!/usr/bin/env python3
"""Gate G1c: changed compiler code is executed by the golden corpus
(README.md, Gates).

  coverage.py run    --build DIR --root CORPUS --home HOME --out OUT [-j N]
  coverage.py check  --profile OUT --base REV --head REV [--baseline FILE]
                     [--accept FILE] [--paths src/llvm_backend src/ir ...]
  coverage.py summary --profile OUT --write FILE [--source-root DIR]

run      captures the corpus with a coverage build of quidra_golden_dump
         (configured with -DCMAKE_CXX_FLAGS="-fprofile-instr-generate
         -fcoverage-mapping"), one .profraw per process, merges them and
         exports the region and branch coverage (llvm-cov export) to
         OUT/coverage.json.
check    every line the commit range adds or changes under the compiler
         directories must have executed in each code region that covers part
         of it: the line is cut into the pieces llvm-cov's segments give it
         (the region carried over from the previous line, then one piece per
         segment starting on the line), and a piece holding code (more than
         braces, parentheses, separators, `else` or a comment) whose count is
         zero makes the line unexecuted. A region nested in an executed one
         is judged by its own count, so a change in a branch no input takes
         fails even though the enclosing function ran. Lines in regions the
         preprocessor skipped on this platform are listed, not failed.
         Branch coverage of every function the change touches must not be
         lower than in --baseline, the summary of the base (ir_golden.sh
         coverage writes it from a coverage build of --base-ref). Functions
         the baseline does not know (new or moved names) are listed with
         their coverage; a region of theirs that never executes needs a
         probe or fixture. --accept names a file of `PATH:LINE reason` lines
         for changed lines no input can reach (an internal invariant's
         throw); they are printed with their reason instead of failing.
summary  writes the per-function branch coverage (the baseline file).
         --source-root is the checkout the profiled build was configured
         from (default: this one), so that a build of the base in another
         worktree names its sources relative to that worktree.

Segments are llvm-cov export's [line, column, count, has_count,
is_region_entry, is_gap_region]; columns are 1-based byte offsets.
"""

from __future__ import annotations

import argparse
import bisect
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_PATHS = ["src/llvm_backend", "src/llvm_text", "src/lowering", "src/optimizer", "src/ir"]


def tool(name: str) -> str:
    """The profile tools must match the compiler that instrumented the build:
    Xcode's on macOS (Apple clang), the PATH's (or LLVM_TOOLS) elsewhere."""
    if os.environ.get("LLVM_TOOLS"):
        return str(Path(os.environ["LLVM_TOOLS"]) / name)
    if sys.platform == "darwin":
        try:
            return subprocess.run(["xcrun", "--find", name], check=True,
                                  stdout=subprocess.PIPE, text=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    if shutil.which(name):
        return shutil.which(name)
    for version in range(20, 14, -1):
        if shutil.which(f"{name}-{version}"):
            return f"{name}-{version}"
    raise SystemExit(f"coverage.py: {name} not found")


def run(args) -> None:
    out = Path(args.out).resolve()
    profiles = out / "profiles"
    if out.exists():
        shutil.rmtree(out)
    profiles.mkdir(parents=True)
    binary = Path(args.build).resolve() / "quidra_golden_dump"
    subprocess.run([sys.executable, str(HERE / "capture.py"), "--tool", str(binary), "--root",
                    args.root, "--home", args.home, "--out", str(out / "capture"), "-j",
                    str(args.jobs), "--env", f"LLVM_PROFILE_FILE={profiles}/%p-%m.profraw"],
                   check=True)
    raw = sorted(glob.glob(str(profiles / "*.profraw")))
    merged = out / "merged.profdata"
    listing = out / "profiles.txt"
    listing.write_text("\n".join(raw) + "\n", encoding="utf-8")
    subprocess.run([tool("llvm-profdata"), "merge", "-sparse", f"--input-files={listing}",
                    "-o", str(merged)], check=True)
    with open(out / "coverage.json", "w", encoding="utf-8") as handle:
        subprocess.run([tool("llvm-cov"), "export", "-format=text", f"-instr-profile={merged}",
                        str(binary)], check=True, stdout=handle)
    print(f"coverage.py: {len(raw)} profiles -> {out / 'coverage.json'}")


def load(profile: Path) -> dict:
    return json.loads((profile / "coverage.json").read_text(encoding="utf-8"))["data"][0]


def relative(path: str, root: Path = REPO) -> str:
    try:
        return str(Path(path).resolve().relative_to(root))
    except ValueError:
        return path


def function_summaries(data: dict, root: Path = REPO) -> dict[str, dict]:
    result = {}
    for function in data["functions"]:
        branches = function.get("branches", [])
        sides = 2 * len(branches)
        taken = sum((1 if b[4] > 0 else 0) + (1 if b[5] > 0 else 0) for b in branches)
        result[function["name"]] = {
            "files": [relative(f, root) for f in function["filenames"]],
            "count": function["count"],
            "branch_sides": sides,
            "branch_sides_taken": taken,
            "regions": function["regions"],
        }
    return result


def changed_lines(base: str, head: str, paths: list[str]) -> dict[str, set[int]]:
    text = subprocess.run(["git", "-C", str(REPO), "diff", "--unified=0", "--no-color", base, head,
                           "--", *paths], check=True, stdout=subprocess.PIPE, text=True).stdout
    result: dict[str, set[int]] = defaultdict(set)
    current = None
    for line in text.splitlines():
        if line.startswith("+++ "):
            current = None if line[4:] == "/dev/null" else line[6:]
        match = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", line)
        if match and current:
            start, count = int(match.group(1)), int(match.group(2) or "1")
            result[current].update(range(start, start + count))
    return result


# Text of a piece that carries no code of its own: braces, parentheses,
# separators, `else`, a trailing comment.
NO_CODE = re.compile(r"\belse\b|[{}();,\s]")


def holds_code(piece: str) -> bool:
    return bool(NO_CODE.sub("", piece.split("//", 1)[0]))


def line_pieces(segments: list[list], starts: list[tuple[int, int]], line: int,
                text: bytes) -> list[tuple[str, list]]:
    """(text, segment) for each piece of LINE: the segment in effect at the
    line's first column, then every segment that starts on the line, each
    reaching to the next segment or the end of the line."""
    first = bisect.bisect_left(starts, (line, 0))
    last = bisect.bisect_left(starts, (line + 1, 0))
    pieces = []
    columns = [1] + [segments[i][1] for i in range(first, last)]
    owners = ([segments[first - 1]] if first > 0 else [None]) + segments[first:last]
    for index, owner in enumerate(owners):
        if owner is None:
            continue
        begin = columns[index] - 1
        end = columns[index + 1] - 1 if index + 1 < len(columns) else len(text)
        pieces.append((text[begin:end].decode("utf-8", "replace"), owner))
    return pieces


def head_text(head: str, file: str) -> list[bytes]:
    data = subprocess.run(["git", "-C", str(REPO), "show", f"{head}:{file}"], check=True,
                          stdout=subprocess.PIPE).stdout
    return data.split(b"\n")


def read_accepted(path: str | None) -> dict[str, str]:
    accepted = {}
    if not path:
        return accepted
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        location, _, reason = line.partition(" ")
        if not reason.strip():
            raise SystemExit(f"coverage.py: {path}: `{location}` needs a reason")
        accepted[location] = reason.strip()
    return accepted


def check(args) -> int:
    data = load(Path(args.profile))
    accepted = read_accepted(args.accept)
    functions = function_summaries(data)
    baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8")) if args.baseline else {}
    changes = changed_lines(args.base, args.head, args.paths or DEFAULT_PATHS)
    unexecuted = []
    skipped = []
    missing = []
    touched = {}
    # Code regions per file: (line_start, line_end, count, function).
    regions: dict[str, list[tuple[int, int, int, str]]] = defaultdict(list)
    for name, summary in functions.items():
        for region in summary["regions"]:
            line_start, _, line_end, _, count, file_id, _, kind = region[:8]
            if kind != 0:  # code regions only
                continue
            file = summary["files"][file_id]
            regions[file].append((line_start, line_end, count, name))
    segments_of: dict[str, list[list]] = {}
    for entry in data["files"]:
        segments_of[relative(entry["filename"])] = sorted(entry["segments"],
                                                          key=lambda s: (s[0], s[1]))
    for file, lines in sorted(changes.items()):
        if file not in segments_of:
            # A changed source the coverage build did not instrument (or a
            # build configured from another checkout): no evidence at all.
            if file.endswith((".cpp", ".cc", ".cxx")):
                missing.append(file)
            continue
        segments = segments_of[file]
        starts = [(s[0], s[1]) for s in segments]
        text = head_text(args.head, file)
        for line in sorted(lines):
            for _, _, _, name in (r for r in regions.get(file, []) if r[0] <= line <= r[1]):
                touched[name] = functions[name]
            source = text[line - 1] if line - 1 < len(text) else b""
            zero = False
            not_compiled = False
            for piece, (_, _, count, has_count, entry, gap) in line_pieces(segments, starts, line,
                                                                         source):
                if not holds_code(piece):
                    continue
                if not has_count:
                    not_compiled = not_compiled or bool(entry)
                    continue
                if not gap and count == 0:
                    zero = True
            if zero and f"{file}:{line}" in accepted:
                print(f"  accepted unexecuted {file}:{line}: {accepted[f'{file}:{line}']}")
            elif zero:
                unexecuted.append(f"{file}:{line}")
            elif not_compiled:
                skipped.append(f"{file}:{line}")
    lowered = []
    for name, summary in sorted(touched.items()):
        sides, taken = summary["branch_sides"], summary["branch_sides_taken"]
        now = taken / sides if sides else 1.0
        before = baseline.get(name)
        if before is None:
            print(f"  new or moved function {name}: branch sides {taken}/{sides}")
        elif now + 1e-12 < before:
            lowered.append(f"{name}: {before:.3f} -> {now:.3f}")
    print(f"coverage.py: {sum(len(v) for v in changes.values())} changed lines, "
          f"{len(touched)} touched functions, {len(unexecuted)} changed lines never executed, "
          f"{len(lowered)} functions with lower branch coverage, "
          f"{len(missing)} changed sources without coverage data")
    for item in unexecuted[:200]:
        print(f"  never executed: {item}")
    for item in lowered:
        print(f"  lower branch coverage: {item}")
    for item in missing:
        print(f"  no coverage data: {item}")
    for item in skipped[:200]:
        print(f"  not compiled on this platform (no evidence here): {item}")
    return 1 if unexecuted or lowered or missing else 0


def summary(args) -> None:
    root = Path(args.source_root).resolve() if args.source_root else REPO
    functions = function_summaries(load(Path(args.profile)), root)
    result = {name: (s["branch_sides_taken"] / s["branch_sides"] if s["branch_sides"] else 1.0)
              for name, s in functions.items()
              if any(f.startswith("src/") for f in s["files"])}
    Path(args.write).write_text(json.dumps(result, indent=0, sort_keys=True) + "\n",
                                encoding="utf-8")
    print(f"coverage.py: {len(result)} compiler functions -> {args.write}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    r = commands.add_parser("run")
    r.add_argument("--build", required=True)
    r.add_argument("--root", required=True)
    r.add_argument("--home", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    c = commands.add_parser("check")
    c.add_argument("--profile", required=True)
    c.add_argument("--base", required=True)
    c.add_argument("--head", required=True)
    c.add_argument("--baseline")
    c.add_argument("--accept")
    c.add_argument("--paths", nargs="*")
    s = commands.add_parser("summary")
    s.add_argument("--profile", required=True)
    s.add_argument("--write", required=True)
    s.add_argument("--source-root")
    args = parser.parse_args()
    if args.command == "run":
        run(args)
        return 0
    if args.command == "summary":
        summary(args)
        return 0
    return check(args)


if __name__ == "__main__":
    raise SystemExit(main())
