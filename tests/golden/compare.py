#!/usr/bin/env python3
"""Compare two golden captures (README.md, Capture and comparison).

  compare.py BASE HEAD [--pins] [--common] [--views LIST] [--skip-views LIST]
             [--expect FILE]... [--column-map FILE]... [--list-explained]
             [--diff-lines N]

BASE and HEAD are capture directories (index.tsv plus out/). Every entry and
view of BASE must exist in HEAD with the same sha256 and status; with --pins,
every entry's primary status must also equal its pin in corpus.toml. --common
compares only the entries and views both captures have (a harness change adds
views or entries; the views that existed before must stay identical).

--expect FILE (repeatable) declares the expected differences of a change
(expect.py): an (entry, view) outside them must be identical, one inside
them may differ only where its rules explain every differing line. Status
changes are reported in a section of their own and must each be expected;
without --expect none is. --column-map FILE (repeatable) translates the
source columns of a rewritten corpus (expect.py, source column maps): HEAD
captured from the rewritten sources equals BASE once every column field is
mapped.

On a mismatch it prints the entry, the view, the lines no rule explains or
the first differing line, and a unified diff (first N lines), and exits 1.
"""

from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "scripts"))
import toml_subset  # noqa: E402

sys.path.insert(0, str(HERE))
import expect  # noqa: E402

PRIMARY_VIEW = {"ir", "compile.memory", "fixture", "repl.session"}


def read_index(capture: Path) -> dict[tuple[str, str], tuple[str, str, str]]:
    index = {}
    for line in (capture / "index.tsv").read_text(encoding="utf-8").splitlines():
        if line:
            entry, view, digest, size, status = line.split("\t")
            index[(entry, view)] = (digest, size, status)
    return index


def view_file(capture: Path, entry: str, view: str) -> Path | None:
    for candidate in (capture / "out" / entry / view, capture / "out" / entry / f"{view}.error"):
        if candidate.exists():
            return candidate
    return None


def show_diff(base: Path, head: Path, entry: str, view: str, limit: int) -> None:
    left = view_file(base, entry, view)
    right = view_file(head, entry, view)
    if left is None or right is None:
        print(f"    (no output file on {'base' if left is None else 'head'})")
        return
    a = left.read_text(encoding="utf-8", errors="replace").splitlines()
    b = right.read_text(encoding="utf-8", errors="replace").splitlines()
    for number, (x, y) in enumerate(zip(a, b), 1):
        if x != y:
            print(f"    first difference at line {number}:\n    - {x[:300]}\n    + {y[:300]}")
            break
    else:
        print(f"    first difference: line counts {len(a)} vs {len(b)}")
    diff = list(difflib.unified_diff(a, b, f"base/{entry}/{left.name}", f"head/{entry}/{right.name}",
                                     lineterm="", n=2))
    for line in diff[:limit]:
        print("    " + line[:400])
    if len(diff) > limit:
        print(f"    ... {len(diff) - limit} more diff lines")


def read_view(capture: Path, entry: str, view: str) -> str:
    path = view_file(capture, entry, view)
    return path.read_text(encoding="utf-8", errors="replace") if path else ""


def print_explained(explained: list, accepted: list[str], every: bool) -> None:
    """The summary a reviewer checks against the change's declared classes."""
    classes: dict[str, int] = {}
    by_rule: dict[str, int] = {}
    for _, _, verdict in explained:
        for name, lines in verdict.classes.items():
            classes[name] = classes.get(name, 0) + lines
        for rule in verdict.rules:
            by_rule[rule] = by_rule.get(rule, 0) + 1
    print(f"expected differences: {len(explained)} (entry, view) pairs explained, "
          f"{len(accepted)} entries added or removed as declared")
    for name in sorted(classes, key=lambda n: (len(n), n)):
        print(f"  {name}: {classes[name]} lines")
    for rule in sorted(by_rule):
        print(f"  rule {rule}: {by_rule[rule]} pairs")
    review = [(e, v) for e, v, verdict in explained if verdict.accepted_any]
    if review:
        print(f"accepted for review (declared lists, {len(review)}):")
        for entry, view in review:
            print(f"  {entry} [{view}]")
    if every:
        for entry, view, verdict in explained:
            detail = ", ".join(f"{n}={c}" for n, c in sorted(verdict.classes.items()))
            print(f"  explained {entry} [{view}]: {detail}")
        for item in accepted:
            print(f"  {item}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("base")
    parser.add_argument("head")
    parser.add_argument("--pins", action="store_true", help="check statuses against corpus.toml")
    parser.add_argument("--common", action="store_true", help="compare shared entries/views only")
    parser.add_argument("--views", help="comma-separated views to compare")
    parser.add_argument("--skip-views", help="comma-separated views not to compare")
    parser.add_argument("--cli-status", action="store_true",
                        help="for views that failed on both sides, compare the status only")
    parser.add_argument("--expect", action="append", default=[], metavar="FILE",
                        help="expected differences (repeatable; their rules add up)")
    parser.add_argument("--column-map", action="append", default=[], metavar="FILE",
                        help="source column map of a rewritten corpus (repeatable)")
    parser.add_argument("--list-explained", action="store_true",
                        help="list every explained (entry, view) with its classes")
    parser.add_argument("--diff-lines", type=int, default=200)
    parser.add_argument("--max-reports", type=int, default=20)
    args = parser.parse_args()

    base = Path(args.base)
    head = Path(args.head)
    left = read_index(base)
    right = read_index(head)
    views = set(args.views.split(",")) if args.views else None
    skipped = set(args.skip_views.split(",")) if args.skip_views else set()
    expectations = expect.Expectations(args.expect, args.column_map) \
        if args.expect or args.column_map else None
    problems = 0
    reported = 0
    status_changes: list[tuple[str, str, str, str]] = []
    explained: list[tuple[str, str, expect.Verdict]] = []
    accepted: list[str] = []

    def report(message: str, entry: str | None = None, view: str | None = None) -> None:
        nonlocal problems, reported
        problems += 1
        if reported < args.max_reports:
            print(message)
            if entry is not None and view is not None:
                show_diff(base, head, entry, view, args.diff_lines)
        reported += 1

    keys = sorted(set(left) | set(right))
    for key in keys:
        entry, view = key
        if (views and view not in views) or view in skipped:
            continue
        if key not in right:
            if expectations and expectations.entry_removed(entry):
                accepted.append(f"removed {entry} [{view}]")
            elif not args.common:
                report(f"MISSING in head: {entry} [{view}]")
            continue
        if key not in left:
            if expectations and expectations.entry_added(entry, view, base, head):
                accepted.append(f"added {entry} [{view}]")
            elif not args.common:
                report(f"ADDED in head: {entry} [{view}]")
            continue
        (a_digest, _, a_status), (b_digest, _, b_status) = left[key], right[key]
        if a_status != b_status:
            status_changes.append((entry, view, a_status, b_status))
        elif args.cli_status and a_status.startswith("error:"):
            continue
        elif a_digest != b_digest:
            if expectations is None:
                report(f"DIFF {entry} [{view}]", entry, view)
                continue
            verdict = expectations.check(entry, view, read_view(base, entry, view),
                                         read_view(head, entry, view), base, head)
            if verdict.explained:
                explained.append((entry, view, verdict))
                continue
            report(f"DIFF {entry} [{view}]: not explained by "
                   + (", ".join(verdict.rules) if verdict.rules else "any rule"), entry, view)
            if reported <= args.max_reports:
                for line in verdict.residual[:20]:
                    print(f"    unexplained {line}")
                if len(verdict.residual) > 20:
                    print(f"    ... {len(verdict.residual) - 20} more unexplained lines")

    # Status changes, apart from the content differences (Gstat).
    if status_changes:
        print(f"status changes ({len(status_changes)}):")
        for entry, view, before, after in status_changes:
            known = expectations is not None and expectations.status_expected(
                entry, view, before, after)
            print(f"  {'expected  ' if known else 'UNEXPECTED'} {entry} [{view}]: "
                  f"{before} -> {after}")
        for entry, view, before, after in status_changes:
            if expectations is None or not expectations.status_expected(entry, view, before,
                                                                           after):
                report(f"STATUS {entry} [{view}]: {before} -> {after}", entry, view)

    if expectations is not None:
        print_explained(explained, accepted, args.list_explained)

    if args.pins:
        pins = toml_subset.load(HERE / "corpus.toml")
        for pin in pins.get("entry", []):
            for view in PRIMARY_VIEW:
                found = right.get((pin["name"], view))
                if found and found[2] != pin["status"]:
                    report(f"PIN {pin['name']} [{view}]: pinned {pin['status']}, head {found[2]}")

    compared = sum(1 for k in keys if (not views or k[1] in views) and k[1] not in skipped
                   and k in left and k in right)
    if problems:
        print(f"compare.py: {problems} mismatch(es) over {compared} compared views")
        return 1
    if expectations is not None and (explained or accepted or status_changes):
        print(f"compare.py: every difference expected ({compared} views)")
        return 0
    print(f"compare.py: identical ({compared} views)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
