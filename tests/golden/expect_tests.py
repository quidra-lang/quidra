#!/usr/bin/env python3
"""Self-test of the expected-difference rules of compare.py (expect.py).

  expect_tests.py [--tool QUIDRA_GOLDEN_DUMP]

Builds small capture directories (index.tsv plus out/) and checks that
compare.py --expect accepts exactly the differences each rule kind explains
and reports the rest, with status changes in a section of their own. With
--tool, it also rewrites a probe synthetically (characters inserted and
deleted), captures the original and the rewritten probe with the golden
tool, and checks that the column map of the rewrite makes them equal.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPARE = HERE / "compare.py"
sys.path.insert(0, str(HERE))
import expect  # noqa: E402

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def write_capture(directory: Path, views: dict[tuple[str, str], tuple[str, str]]) -> Path:
    """views: (entry, view) -> (text, status)."""
    rows = []
    for (entry, view), (text, status) in sorted(views.items()):
        target = directory / "out" / entry
        target.mkdir(parents=True, exist_ok=True)
        name = view if status == "ok" else f"{view}.error"
        data = text.encode("utf-8")
        (target / name).write_bytes(data)
        rows.append(f"{entry}\t{view}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{status}")
    (directory / "index.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    return directory


def compare(base: dict, head: dict, rules: dict | None, *extra: str) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        left = write_capture(root / "base", base)
        right = write_capture(root / "head", head)
        command = [sys.executable, str(COMPARE), str(left), str(right), *extra]
        if rules is not None:
            (root / "expect.json").write_text(json.dumps(rules), encoding="utf-8")
            command += ["--expect", str(root / "expect.json")]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        return result.returncode, result.stdout


LLVM_BASE = """\
@.msg.bounds = private unnamed_addr constant [6 x i8] c"bound\\00"
declare void @quidra_runtime_fail_at(ptr, ptr, i64, i64)
define void @main() {
entry:
  %v1 = call ptr @quidra_array_slot(ptr %a, i64 %i, i64 8, i64 3, i64 5)
  br i1 %bad, label %fail, label %ok
fail:
  call void @quidra_runtime_fail_at(ptr @.code.x, ptr @.msg.bounds, i64 3, i64 5)
  unreachable
ok:
  ret void
}
"""


def text_case(name: str, base: str, head: str, rule: dict | None, expected: int,
              view: str = "llvm", must: str = "") -> None:
    code, output = compare({("e.qui", view): (base, "ok")}, {("e.qui", view): (head, "ok")},
                           None if rule is None else {"rules": [rule]})
    check(code == expected, f"{name}: exit {code}, expected {expected}\n{output}")
    if must:
        check(must in output, f"{name}: output lacks {must!r}\n{output}")


def column_map_cases() -> None:
    edits = expect.FileEdits()
    edits.add(3, 5, 4)    # four characters inserted before column 5
    edits.add(3, 20, -2)  # columns 20 and 21 deleted
    check(edits.columns(3, 2) == {2}, "before an insertion")
    check(edits.columns(3, 5) == {5, 9}, "at an insertion: both translations")
    check(edits.columns(3, 7) == {11}, "after an insertion")
    check(edits.columns(3, 21) == {24}, "inside a deletion")
    check(edits.columns(3, 30) == {32}, "after both")
    check(edits.columns(4, 30) == {30}, "another line")
    check(edits.offsets(4, 1, 100) == {102}, "offsets move by the edits of earlier lines")

    base = {("e.qui", "llvm"): ("  call ptr @quidra_array_slot(ptr %a, i64 %i, i64 8, i64 3, i64 9)\n"
                                "!7 = !DILocation(line: 3, column: 9, scope: !4)\n", "ok"),
            ("e.qui", "diagnostics"): ("CompileError\nTYPE_MISMATCH 3:9-3:12 @40-43 \"m\"\n",
                                       "error:TYPE_MISMATCH")}
    head = {("e.qui", "llvm"): ("  call ptr @quidra_array_slot(ptr %a, i64 %i, i64 8, i64 3, i64 13)\n"
                                "!7 = !DILocation(line: 3, column: 13, scope: !4)\n", "ok"),
            ("e.qui", "diagnostics"): ("CompileError\nTYPE_MISMATCH 3:13-3:16 @44-47 \"m\"\n",
                                       "error:TYPE_MISMATCH")}
    with tempfile.TemporaryDirectory() as temporary:
        maps = Path(temporary)
        (maps / "right.tsv").write_text("e.qui\t3\t5\t4\n", encoding="utf-8")
        (maps / "line.tsv").write_text("e.qui\t2\t5\t4\n", encoding="utf-8")
        (maps / "other.tsv").write_text("f.qui\t3\t5\t4\n", encoding="utf-8")
        (maps / "bad.tsv").write_text("e.qui\t3\t5\n", encoding="utf-8")
        for name, expected in (("right", 0), ("line", 1), ("other", 1), ("bad", 1)):
            code, output = compare(base, head, None, "--column-map", str(maps / f"{name}.tsv"))
            check(code == expected, f"column map {name}: exit {code}, expected {expected}\n{output}")
        code, output = compare(base, head, None)
        check(code == 1, f"columns without a map: exit {code}\n{output}")
        # A changed operand is not a column, even where a map applies.
        operand = dict(head)
        operand[("e.qui", "llvm")] = (head[("e.qui", "llvm")][0].replace("i64 8,", "i64 16,"), "ok")
        code, output = compare(base, operand, None, "--column-map", str(maps / "right.tsv"))
        check(code == 1, f"column map explains an operand: exit {code}\n{output}")
        # The map applies to the entry's sources through its deps view, and
        # from an expectation file.
        imported = {("main.qui", "llvm"): base[("e.qui", "llvm")],
                    ("main.qui", "deps"): ('root "/c/main.qui"\nsource "/c/e.qui" 10 ab\n', "ok")}
        moved = {("main.qui", "llvm"): head[("e.qui", "llvm")],
                 ("main.qui", "deps"): imported[("main.qui", "deps")]}
        rules = {"column_maps": [str(maps / "right.tsv")]}
        code, output = compare(imported, moved, rules)
        check(code == 0 and "columns: " in output, f"column map through deps: {code}\n{output}")


def apply_edits(source: str, edits: list[tuple[int, int, str | int]]) -> tuple[str, list[str]]:
    """Inserts text (a string) or deletes characters (a count) at 1-based
    (line, column) positions of the original; returns the rewritten source
    and the column map's lines."""
    lines = source.split("\n")
    rows = []
    for number in sorted({line for line, _, _ in edits}):
        text = lines[number - 1]
        for _, column, change in sorted((e for e in edits if e[0] == number),
                                        key=lambda e: e[1], reverse=True):
            if isinstance(change, str):
                text = text[:column - 1] + change + text[column - 1:]
                rows.append(f"e.qui\t{number}\t{column}\t{len(change)}")
            else:
                text = text[:column - 1] + text[column - 1 + change:]
                rows.append(f"e.qui\t{number}\t{column}\t{-change}")
        lines[number - 1] = text
    return "\n".join(lines), rows


PROBE = """\
int[] values = [10, 20, 30]
int  index  =  2
int total = values[index] + values[0]
print(total)
print(NL)
string text = "abc"
print(text[index])
print(NL)
"""

PROBE_EDITS = [(2, 5, 1), (2, 12, 1), (2, 15, 1), (3, 20, " "), (3, 25, " "),
               (7, 7, "  "), (7, 12, "   ")]

BROKEN = """\
int  count  =  "three"
print(count)
"""

BROKEN_EDITS = [(1, 5, 1), (1, 12, 1), (1, 15, 1)]


def synthetic_rewrite(tool: Path) -> None:
    """Capture a probe and a broken program, rewrite both at one root with
    characters inserted and deleted, capture again: with the rewrite's
    column map every compared view is equal, without it some are not."""
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary).resolve()
        root = work / "root"
        root.mkdir()
        home = work / "home"
        entries = []
        for name in ("e.qui", "broken/e.qui"):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            entries.append("\t".join([name, "file", str(path), str(path.parent), str(root)]))
        (root / "entries.tsv").write_text("\n".join(entries) + "\n", encoding="utf-8")
        rows = []
        for name, source, edits in (("e.qui", PROBE, PROBE_EDITS),
                                    ("broken/e.qui", BROKEN, BROKEN_EDITS)):
            rewritten, map_rows = apply_edits(source, edits)
            check(rewritten != source, f"{name}: the rewrite changes nothing")
            (root / name).write_text(source, encoding="utf-8")
            (work / f"{name.replace('/', '_')}.rewritten").write_text(rewritten, encoding="utf-8")
            rows += [row.replace("e.qui", name, 1) for row in map_rows]
        (work / "map.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")

        def capture(out: str) -> Path:
            result = subprocess.run(
                [sys.executable, str(HERE / "capture.py"), "--tool", str(tool), "--root", str(root),
                 "--home", str(home), "--out", str(work / out), "-j", "2"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check(result.returncode == 0, f"capture {out}: {result.stdout}")
            return work / out

        base = capture("base")
        for name in ("e.qui", "broken/e.qui"):
            (root / name).write_text(
                (work / f"{name.replace('/', '_')}.rewritten").read_text(encoding="utf-8"),
                encoding="utf-8")
        head = capture("head")
        # inspect and deps hold the source text and its hash, not columns.
        skip = ["--skip-views", "inspect,deps"]
        # Rewriting source text necessarily changes the source-table revision
        # and byte count. Declare that metadata as K10; column maps must
        # still explain every other difference, including source locations.
        metadata = work / "source-metadata.json"
        metadata.write_text(json.dumps({"rules": [{
            "id": "rewritten source metadata",
            "entries": ["e.qui", "broken/e.qui"],
            "views": ["ir.full", "ir.lowered", "llvm", "llvm.debug",
                      "llvm.lowered", "llvm.lowered.debug", "repl"],
            "classes": ["K10"],
        }]}), encoding="utf-8")
        command = [sys.executable, str(COMPARE), str(base), str(head),
                   "--expect", str(metadata), *skip]
        plain = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
        check(plain.returncode == 1, "the rewrite moves no column in any view:\n" + plain.stdout)
        mapped = subprocess.run(command + ["--column-map", str(work / "map.tsv")],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        check(mapped.returncode == 0, "the column map leaves differences:\n" + mapped.stdout)
        for view in ("ir.full", "llvm", "llvm.debug", "diagnostics"):
            check(f"[{view}]" in plain.stdout or view not in ("ir.full", "llvm", "llvm.debug"),
                  f"the rewrite moves no column in {view}:\n{plain.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", help="quidra_golden_dump, for the synthetic rewrite")
    args = parser.parse_args()
    column_map_cases()
    if args.tool:
        synthetic_rewrite(Path(args.tool).resolve())

    # Identical captures, and a difference with no expectation.
    text_case("identical", LLVM_BASE, LLVM_BASE, None, 0, must="identical")
    changed = LLVM_BASE.replace('c"bound\\00"', 'c"bounds\\00"').replace("[6 x i8]", "[7 x i8]")
    text_case("no expectation", LLVM_BASE, changed, None, 1, must="DIFF e.qui [llvm]")

    # K1: a diagnostic constant; only on the covered entry and view.
    text_case("K1", LLVM_BASE, changed, {"entries": ["e.qui"], "classes": ["K1"]}, 0,
              must="K1: 2 lines")
    text_case("K1 other entry", LLVM_BASE, changed, {"entries": ["f.qui"], "classes": ["K1"]}, 1,
              must="not explained by any rule")
    text_case("K1 other view", LLVM_BASE, changed,
              {"views": ["ir"], "classes": ["K1"]}, 1)
    text_case("K2 is not K1", LLVM_BASE, changed, {"classes": ["K2"]}, 1,
              must="unexplained - @.msg.bounds")

    # K2: a runtime declaration added.
    declared = LLVM_BASE.replace("declare void @quidra_runtime_fail_at",
                                 "declare void @quidra_runtime_hold()\n"
                                 "declare void @quidra_runtime_fail_at")
    text_case("K2", LLVM_BASE, declared, {"classes": ["K2"]}, 0)

    # K3: a cold block (it ends in unreachable); kinds restrict it.
    cold = LLVM_BASE.replace("i64 3, i64 5)\n  unreachable",
                             "i64 3, i64 5)\n  call void @flush()\n  unreachable")
    text_case("K3", LLVM_BASE, cold, {"classes": ["K3"]}, 0)
    text_case("K3 kinds", LLVM_BASE, cold, {"classes": [{"class": "K3", "kinds": ["store"]}]}, 1)
    hot = LLVM_BASE.replace("  ret void", "  call void @flush()\n  ret void")
    text_case("K3 hot block", LLVM_BASE, hot, {"classes": ["K3"]}, 1)

    # K4: immediates added at a named callee's calls.
    immediate = LLVM_BASE.replace("@quidra_array_slot(ptr %a, i64 %i, i64 8, i64 3, i64 5)",
                                  "@quidra_array_slot(ptr %a, i64 %i, i64 8, i64 3, i64 5, i64 1)")
    text_case("K4", LLVM_BASE, immediate,
              {"classes": [{"class": "K4", "callees": ["quidra_array_slot"]}]}, 0)
    text_case("K4 other callee", LLVM_BASE, immediate,
              {"classes": [{"class": "K4", "callees": ["quidra_bin_index"]}]}, 1)

    # K11 and K12: location values only.
    moved = LLVM_BASE.replace("ptr @.msg.bounds, i64 3, i64 5)", "ptr @.msg.bounds, i64 3, i64 9)")
    text_case("K11", LLVM_BASE, moved, {"classes": ["K11"]}, 0)
    slot = LLVM_BASE.replace("i64 8, i64 3, i64 5)", "i64 8, i64 3, i64 7)")
    text_case("K12", LLVM_BASE, slot, {"classes": ["K12"]}, 0)
    text_case("K12 shape change", LLVM_BASE, slot.replace("i64 8, i64 3, i64 7)",
                                                         "i64 8, i64 3, i64 7, i64 0)"),
              {"classes": ["K12"]}, 1)

    # K5: a function the base does not define.
    exported = LLVM_BASE + "define i64 @add2(i64 %a) {\nentry:\n  ret i64 %a\n}\n"
    text_case("K5", LLVM_BASE, exported, {"classes": ["K5"]}, 0)

    # K14: call-site debug metadata stripped.
    debug = LLVM_BASE.replace("call void @quidra_runtime_fail_at(ptr @.code.x, ptr @.msg.bounds, "
                              "i64 3, i64 5)",
                              "call void @quidra_runtime_fail_at(ptr @.code.x, ptr @.msg.bounds, "
                              "i64 3, i64 5), !dbg !7") + \
        '!llvm.dbg.cu = !{!0}\n!7 = !DILocation(line: 3, column: 5, scope: !4)\n'
    text_case("K14", LLVM_BASE, debug, {"classes": ["K14"]}, 0)
    text_case("K14 absent", LLVM_BASE, debug, {"classes": ["K1"]}, 1)

    # K15 and K17: error records and hops.
    errors = LLVM_BASE.replace("define void @main() {",
                               "@.quidra.err.0 = private constant [2 x i64] [i64 1, i64 2]\n"
                               "define void @main() {")
    text_case("K15", LLVM_BASE, errors, {"classes": ["K15"]}, 0)
    hop = LLVM_BASE.replace("fail:\n", "fail:\n  %h = call ptr @quidra_error_hop(ptr %e, ptr @.quidra.hop.0)\n")
    text_case("K17", LLVM_BASE, hop, {"classes": ["K17"]}, 0)
    text_case("K16 is not K17", LLVM_BASE, hop, {"classes": ["K16"]}, 1)

    # Token rules: renames, string lengths, permutations, shapes.
    renamed = LLVM_BASE.replace("@.msg.bounds", "@.msg.limits")
    text_case("renames", LLVM_BASE, renamed, {"renames": {"bounds": "limits"}}, 0,
              must="token rules")
    text_case("renames partial token", LLVM_BASE, LLVM_BASE.replace("@.msg.bounds", "@.msg.boundsX"),
              {"renames": {"bounds": "boundsX"}}, 0)
    text_case("string lengths", LLVM_BASE, changed.replace('c"bounds\\00"', 'c"bound\\00"'),
              {"string_lengths": True}, 0)
    tagged = "VariantMake{1, 0, 2}\nVariantMake{3, 1, 4}\n"
    permuted = "VariantMake{1, 1, 2}\nVariantMake{3, 0, 4}\n"
    text_case("permutations", tagged, permuted,
              {"permutations": [{"pattern": r"VariantMake\{\d+, (?P<tag>\d+)",
                                 "map": {"0": "1", "1": "0"}}]}, 0, view="ir.full")
    shaped = LLVM_BASE.replace("  br i1 %bad, label %fail, label %ok",
                               "  %c = icmp ne i1 %bad, 0\n  br i1 %c, label %fail, label %ok")
    text_case("shapes", LLVM_BASE, shaped,
              {"shapes": [{"base": [r"^  br i1 %bad"], "head": [r"^  %c = icmp", r"^  br i1 %c"]}]},
              0, must="shape")

    # ir.full fields: declared positions, K7, K13.
    ir_base = "    3 BinSlice{4, 1, 2, 3}\n    4 Binary{5, \"+\", 1, 2}\n"
    ir_fields = "    3 BinSlice{4, 1, 2, 3, 7, 9}\n    4 Binary{5, \"+\", 1, 2}\n"
    text_case("new_fields", ir_base, ir_fields, {"new_fields": {"BinSlice": [4, 5]}}, 0,
              view="ir.full")
    text_case("new_fields wrong positions", ir_base, ir_fields,
              {"new_fields": {"BinSlice": [1, 2]}}, 1, view="ir.full")
    text_case("K7", ir_base, ir_fields, {"classes": ["K7"]}, 0, view="ir.full")
    ir_line = "    4 Binary{5, \"+\", 1, 2}\n"
    text_case("K13", ir_line, "    4 Binary{5, \"+\", 1, 2, 12, 3}\n", {"classes": ["K13"]}, 0,
              view="ir.full")
    text_case("K13 non-integer", ir_line, "    4 Binary{5, \"+\", 1, 2, 12, none}\n",
              {"classes": ["K13"]}, 1, view="ir.full")
    block = ("function #0 \"f\"\n  block #0 \"entry\"\n    0 LoadLocal{1, \"e\"}\n"
             "    1 Return{1}\nend\n")
    hopped = ("function #0 \"f\"\n  block #0 \"entry\"\n    0 LoadLocal{1, \"e\"}\n"
              "    1 ErrorHop{2, 1, 0}\n    2 Return{1}\nend\n")
    text_case("positions renumbered", block, hopped, {"classes": ["K17"]}, 0, view="ir.full")
    text_case("positions with no class", block, hopped, {"classes": ["K15"]}, 1, view="ir.full")
    text_case("function numbers", "function #0 \"g\"\nend\n",
              "function #0 \"f\"\nend\nfunction #1 \"g\"\nend\n",
              {"shapes": [{"base": [], "head": ["^function #", "^end$"]}]}, 0, view="ir.full")
    text_case("function field", "function #0 \"f\"\n  entrypoint 0\n",
              "function #0 \"f\"\n  entrypoint 0\n  c_export_symbol none\n",
              {"classes": ["K7"]}, 0, view="ir.full")

    # K6: diagnostics of named codes; K10: the stderr of a failing run.
    diag_base = "CompileError\nINDEX_BOUNDS 1:2-1:5 @1-4 \"old\"\n"
    diag_head = "CompileError\nINDEX_BOUNDS 1:2-1:5 @1-4 \"new\"\n"
    text_case("K6", diag_base, diag_head, {"classes": [{"class": "K6", "codes": ["INDEX_BOUNDS"]}]},
              0, view="diagnostics")
    text_case("K6 other code", diag_base, diag_head,
              {"classes": [{"class": "K6", "codes": ["TYPE_MISMATCH"]}]}, 1, view="diagnostics")
    run_base = "exit 101\nstdout 2 bytes\nok\nstderr 10 bytes\nerror at 3:5\n"
    run_head = "exit 101\nstdout 2 bytes\nok\nstderr 24 bytes\nerror at main.qui:3:5\n  3 | x\n"
    text_case("K10", run_base, run_head, {"classes": ["K10"]}, 0, view="run")
    text_case("K10 stdout", run_base, run_head.replace("\nok\n", "\nko\n"), {"classes": ["K10"]}, 1,
              view="run")

    # any: a declared list, accepted for review.
    text_case("any", LLVM_BASE, hot, {"entries": ["e.qui"], "views": ["llvm"], "any": True}, 0,
              must="accepted for review")

    # Census predicates select the covered entries.
    census_base = "stages lowered=1 optimized=1\nalt 7:Clone 2 1\nevent regions - 3\n"
    census_none = "stages lowered=1 optimized=1\nalt 7:Clone 0 0\nevent regions - 0\n"
    for census, expected in ((census_base, 0), (census_none, 1)):
        base = {("e.qui", "llvm"): (LLVM_BASE, "ok"), ("e.qui", "census"): (census, "ok")}
        head = {("e.qui", "llvm"): (changed, "ok"), ("e.qui", "census"): (census, "ok")}
        code, output = compare(base, head, {"rules": [{"census": ["Clone > 0", "event.regions >= 3"],
                                                       "views": ["llvm"], "classes": ["K1"]}]})
        check(code == expected, f"census predicate ({expected}): exit {code}\n{output}")
    values = expect.census_values(census_base)
    check(values == {"Clone.lowered": 2, "Clone": 1, "event.regions": 3},
          f"census values: {values}")

    # Status changes: their own section, each expected or not.
    base = {("e.qui", "ir"): ("x\n", "ok"), ("f.qui", "ir"): ("y\n", "ok")}
    head = {("e.qui", "ir"): ("CompileError\n", "error:SHADOWING"), ("f.qui", "ir"): ("y\n", "ok")}
    code, output = compare(base, head, None)
    check(code == 1 and "status changes (1):" in output and "UNEXPECTED e.qui [ir]" in output,
          f"status without expectation: {code}\n{output}")
    code, output = compare(base, head, {"status": [{"entry": "e.qui", "views": ["ir"], "from": "ok",
                                                    "to": "error:SHADOWING"}]})
    check(code == 0 and "expected   e.qui [ir]: ok -> error:SHADOWING" in output,
          f"expected status: {code}\n{output}")
    code, output = compare(base, head, {"status": [{"entry": "e.qui", "to": "error:PARSE_ERROR"}]})
    check(code == 1, f"status with another code: {code}\n{output}")

    # Added and removed entries.
    base = {("e.qui", "ir"): ("x\n", "ok")}
    head = {("e.qui", "ir"): ("x\n", "ok"), ("probes/new.qui", "ir"): ("y\n", "ok")}
    code, output = compare(base, head, {"rules": [{"entries": ["probes/*"], "classes": ["K8"]}]})
    check(code == 0, f"K8: {code}\n{output}")
    code, output = compare(base, head, {"rules": [{"entries": ["other/*"], "classes": ["K8"]}]})
    check(code == 1 and "ADDED in head: probes/new.qui" in output, f"K8 other: {code}\n{output}")
    code, output = compare(head, base, {"removed": ["probes/*"]})
    check(code == 0, f"removed: {code}\n{output}")

    # Malformed expectation files are refused.
    for broken in ({"rules": [{"classes": ["K9"]}]}, {"rules": [{"entry": ["x"]}]},
                   {"status": [{"view": "ir"}]}, {"extra": []}):
        code, output = compare({("e.qui", "ir"): ("x\n", "ok")}, {("e.qui", "ir"): ("x\n", "ok")},
                               broken)
        check(code != 0, f"malformed {broken} accepted\n{output}")

    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL: {failure}")
        print(f"expect_tests: {len(FAILURES)} failure(s)")
        return 1
    print("expect_tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
