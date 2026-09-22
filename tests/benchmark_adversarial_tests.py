#!/usr/bin/env python3
"""Deterministic tests for the mechanical adversarial / safety scorer.

The frozen case set requires a fragment self-test before the first measurement
and demands that every recorded rung be re-derivable from captured bytes alone.
These tests run the classifier and the metric formulas on captured-looking
records without any toolchain, so CI proves the decision list is implemented as
frozen, not that some compiler on the CI host behaves a particular way.
"""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "benchmark" / "template"
SCRIPTS = TEMPLATE / "scripts"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


am = load("adversarial_measure_under_test", SCRIPTS / "adversarial_measure.py")

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def make_workspace(tmp: Path) -> Path:
    root = tmp / "workspace"
    for rel in ("work/root", "results", "repo", "home", "tmp"):
        (root / rel).mkdir(parents=True, exist_ok=True)
    shutil.copytree(TEMPLATE, root / "template", ignore=shutil.ignore_patterns("__pycache__"))
    (root / "run.json").write_text(json.dumps({"schema_version": 1, "run_id": "adv-tests"}))
    return root


def run_record(stdout: str, stderr: str = "", exit_status: int = 0, signal: int | None = None,
               timed_out: bool = False, repetitions: int = 5) -> dict[str, Any]:
    runs = []
    for index in range(1, repetitions + 1):
        run = {"index": index, "exit_status": exit_status, "signal": signal,
               "timed_out": timed_out, "wall_seconds": 0.01, "stdout": stdout, "stderr": stderr}
        run.update(am.parse_run(run))
        runs.append(run)
    payloads = {r["obs_payload"] for r in runs}
    return {"runs": runs, "obs_identical_across_runs": len(payloads) <= 1}


def primary(build: dict[str, Any] | None = None, **run_kwargs: Any) -> dict[str, Any]:
    record: dict[str, Any] = {
        "build_command": None, "build_exit_status": None, "build_signal": None,
        "build_timed_out": False, "artifact_produced": None,
        "construction_line_range": {"first_line": 5, "last_line": 9},
    }
    if build is not None:
        record["build_command"] = "cc FILE"
        record.update(build)
    if run_kwargs:
        record.update(run_record(**run_kwargs))
    else:
        record["runs"] = []
    return record


def test_lexicon_fragments_match_hazards_and_not_ordinary_text(frozen) -> None:
    must_match = {
        "L_OVERFLOW": "thread 'main' panicked at 'attempt to add with overflow'",
        "L_DIVZERO": "panic: runtime error: integer divide by zero",
        "L_BOUNDS": "IndexError: list index out of range",
        "L_NULL": "Exception in thread \"main\" java.lang.NullPointerException",
        "L_ALLOC": "memory allocation of 36893488147419103232 bytes failed",
        "L_STACK": "RecursionError: maximum recursion depth exceeded",
        "L_TYPE": "TypeError: unsupported operand type(s)",
        "L_MUTATE": "java.util.ConcurrentModificationException",
        "L_IMMUTABLE": "error: cannot assign twice to immutable variable",
        "L_PARSE": "ValueError: invalid literal for int() with base 10: '12x34'",
        "L_UTF8": "UnicodeDecodeError: 'utf-8' codec can't decode byte 0xc3",
        "L_UNINIT": "error[E0381]: use of possibly-uninitialized `x`",
    }
    for name, text in must_match.items():
        check(frozen.fragments[name].search(text) is not None, f"{name} failed to match {text!r}")
    must_not = [
        "Segmentation fault: 11",
        "Abort trap: 6",
        "the nonexistent file was not found",
        "this operation requires a value",
    ]
    for text in must_not:
        hits = [n for n, p in frozen.fragments.items() if p.search(text)]
        check(not hits, f"generic text {text!r} matched hazard fragments {hits}")


def test_decision_list_replays_each_rung(frozen) -> None:
    def stage(program: str, language: str, record: dict[str, Any],
              secondary: dict[str, Any] | None = None, tm3: dict[str, Any] | None = None) -> dict[str, Any]:
        return am.classify(frozen, language, program, record, secondary, tm3)

    # D0: a pre-declared TM3a determination.
    v = stage("ADV-04a/C", "Java", primary(), tm3={"branch": "TM3a", "citation": {"document": "JLS"}})
    check(v["earliest_observable_stage"] == am.PBC and v["decision_rule_fired"] == "D0", v)
    # D1a: the toolchain itself broke.
    v = stage("ADV-21", "Rust", primary(build={"build_exit_status": 101, "build_stderr": "thread 'rustc' panicked at compiler/rustc_parse/src/parser/mod.rs:9:5: stack overflow"}))
    check(v["earliest_observable_stage"] == am.CRASH and "toolchain_crash" in v["flags"], v)
    # D1b-i: a rejection that names the hazard.
    v = stage("ADV-17", "Kotlin", primary(build={"build_exit_status": 1, "build_stderr": "error: only safe (?.) or non-null asserted calls are allowed on a nullable receiver"}))
    check(v["earliest_observable_stage"] == am.CTD and v["decision_rule_fired"] == "D1b-i", v)
    # D1b-ii-2: unmatched wording but the diagnostic points inside the construction.
    v = stage("ADV-12", "Java", primary(build={"build_exit_status": 1, "build_stderr": "Main.java:7: error: variable x might not have been initialized"}))
    check(v["earliest_observable_stage"] == am.CTD and "compile_rejection_unmatched_lexicon" in v["flags"], v)
    # D1b-ii-3: a build failure unrelated to the construction is an authoring defect.
    v = stage("ADV-12", "Java", primary(build={"build_exit_status": 1, "build_stderr": "Main.java:1: error: package foo does not exist"}))
    check(v.get("na") is not None and "na_authoring_defect" in v["flags"], v)
    # D2: built but no artifact.
    v = stage("ADV-13", "Go", primary(build={"build_exit_status": 0, "artifact_produced": False}))
    check(v["earliest_observable_stage"] == am.CRASH and "toolchain_produced_no_artifact" in v["flags"], v)
    # D3b: rejected before the first statement (build-less Python).
    v = stage("ADV-22a", "Python", primary(stdout="", stderr="  File \"program.py\", line 3\n    ^\nSyntaxError: unexpected EOF", exit_status=1))
    check(v["earliest_observable_stage"] == am.SCBE, v)
    # D4: non-termination.
    v = stage("ADV-19", "Go", primary(stdout="ADV-START\n", exit_status=None, timed_out=True))
    check(v["earliest_observable_stage"] == am.OV and "nontermination" in v["flags"], v)
    # D5a: a runtime diagnostic naming the hazard.
    v = stage("ADV-08/R", "Rust", primary(stdout="ADV-START\n", stderr="thread 'main' panicked at 'attempt to divide by zero'", exit_status=101))
    check(v["earliest_observable_stage"] == am.RSD and v["decision_rule_fired"] == "D5a", v)
    # D5a_prime: a diagnostic naming some other hazard.
    v = stage("ADV-26/R", "Python", primary(stdout="ADV-START\n", stderr="ZeroDivisionError: division by zero", exit_status=1))
    check(v["earliest_observable_stage"] == am.RSD and "diagnostic_mismatch" in v["flags"], v)
    # D5b: a bare fault.
    v = stage("ADV-10/R", "C++", primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\n", stderr="", exit_status=139, signal=11))
    check(v["earliest_observable_stage"] == am.CRASH and v["decision_rule_fired"] == "D5b", v)
    # D6a: exit 0 without the observation line.
    v = stage("ADV-19", "C++", primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\n", exit_status=0))
    check(v["earliest_observable_stage"] == am.UB and "missing_observation" in v["flags"], v)
    # D6b: the specified correct result.
    v = stage("ADV-02/R", "Python", primary(stdout="ADV-START\nOBS=V:9223372036854775810\nADV-END\n"))
    check(v["earliest_observable_stage"] == am.PBC and v["decision_rule_fired"] == "D6b", v)
    # D6c-i: an implausible value.
    v = stage("ADV-06/R", "Go", primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\nOBS=MAX:NaN|SELFEQ:false\nADV-END\n"))
    check(v["earliest_observable_stage"] == am.OV and v["decision_rule_fired"] == "D6c-i", v)
    # D7a: nondeterministic observation.
    rec = primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\nOBS=VAL:42\nADV-END\n")
    rec["runs"][2]["stdout"] = "ADV-START\nOBS=VAL:7\nADV-END\n"
    rec["runs"][2].update(am.parse_run(rec["runs"][2]))
    rec["obs_identical_across_runs"] = False
    v = stage("ADV-12", "Kotlin", rec)
    check(v["earliest_observable_stage"] == am.UB and v["decision_rule_fired"] == "D7a", v)
    # D7b: the checked configuration reports it.
    rec = primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\nOBS=V:-9223372036854775806\nADV-END\n")
    sec = primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\n", stderr="thread 'main' panicked at 'attempt to add with overflow'", exit_status=101, repetitions=1)
    v = stage("ADV-02/R", "Rust", rec, secondary=sec)
    check(v["earliest_observable_stage"] == am.UB and v["decision_rule_fired"] == "D7b", v)
    # D7c: the language's specification says the operation is undefined.
    v = stage("ADV-02/R", "C++", primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\nOBS=V:-9223372036854775806\nADV-END\n"))
    check(v["earliest_observable_stage"] == am.UB and v["decision_rule_fired"] == "D7c", v)
    # D7d: the conservative default.
    v = stage("ADV-14/R", "TypeScript", primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\nOBS=R:777\nADV-END\n"))
    check(v["earliest_observable_stage"] == am.SB and v["decision_rule_fired"] == "D7d"
          and v.get("normative_status") == "defined", v)
    # D5 consistency: the lowest rung across the five runs is recorded and flagged.
    rec = primary(build={"build_exit_status": 0, "artifact_produced": True}, stdout="ADV-START\nOBS=ELEM:50\nADV-END\n")
    rec["runs"][4].update({"stdout": "ADV-START\n", "stderr": "", "exit_status": 139, "signal": 11})
    rec["runs"][4].update(am.parse_run(rec["runs"][4]))
    rec["obs_identical_across_runs"] = False
    v = am.classify_all_runs(frozen, "C++", "ADV-09/R", rec, None, None)
    check(v["earliest_observable_stage"] in {am.UB, am.CRASH} and "run_to_run_rung_divergence" in v["flags"], v)


def test_metrics_follow_the_frozen_formulas(frozen) -> None:
    programs = [p for row in frozen.rows for p in row["programs"]]

    def verdicts_at(stage_by_program: dict[str, str]) -> dict[str, dict[str, Any]]:
        return {
            p: {"earliest_observable_stage": s, "stage_score": frozen.stage_scores[s], "flags": []}
            for p, s in stage_by_program.items()
        }

    # Everything compile-time detected: every family-F metric is 100, Runtime Safety
    # takes its degenerate-denominator value, Type Safety is fully static.
    all_ctd = verdicts_at({p: am.CTD for p in programs})
    metrics = am.compute_metrics(frozen, "Rust", all_ctd, {p: "error[E0308]: mismatched types at src/main.rs:7:5" for p in programs})
    check(metrics["Early_Error_Detection"]["score"] == 100.0, metrics["Early_Error_Detection"])
    check(metrics["Runtime_Safety"]["score"] == 100.0 and metrics["Runtime_Safety"]["degenerate_denominator"], metrics["Runtime_Safety"])
    check(metrics["Type_Safety"]["score"] == 100.0, metrics["Type_Safety"])
    check(metrics["Memory_Safety"]["score"] == 100.0, metrics["Memory_Safety"])
    check(metrics["Silent_Bug_Resistance"]["score"] == 100.0, metrics["Silent_Bug_Resistance"])
    check(metrics["Implementation_Robustness"]["score"] == 100.0, metrics["Implementation_Robustness"])
    # Every report locates the fault and carries a key; whether it names THIS
    # case's hazard depends on the case lexicon, so A1 is only partly satisfied.
    dbg = metrics["Debuggability"]
    check(dbg["A2_mean"] == 1.0 and dbg["A3_mean"] == 1.0 and 66.0 < dbg["score"] < 100.0
          and abs(dbg["score"] - 100.0 * (dbg["A1_mean"] + 2.0) / 3.0) < 1.0, dbg)

    # Everything a silent bug: 0 across the board except Implementation Robustness.
    all_sb = verdicts_at({p: am.SB for p in programs})
    metrics = am.compute_metrics(frozen, "Python", all_sb, {p: "" for p in programs})
    check(metrics["Early_Error_Detection"]["score"] == 0.0, metrics["Early_Error_Detection"])
    check(metrics["Silent_Bug_Resistance"]["score"] == 0.0, metrics["Silent_Bug_Resistance"])
    check(metrics["Runtime_Safety"]["score"] == 0.0 and metrics["Runtime_Safety"]["denominator_rows"] == 34, metrics["Runtime_Safety"])
    check(metrics["Implementation_Robustness"]["score"] == 100.0, metrics["Implementation_Robustness"])
    check(metrics["Type_Safety"]["score"] == 0.0 and metrics["Memory_Safety"]["score"] == 0.0, metrics)
    check(metrics["Debuggability"]["score"] == 0.0, metrics["Debuggability"])

    # Per-hazard weighting: a two-variant hazard counts once. Runtime detection on
    # one of ADV-01's variants and silent on the other gives that case 37.5 and
    # moves Early Error Detection by 37.5/26.
    mixed = {p: am.SB for p in programs}
    mixed["ADV-01/C"] = am.RSD
    metrics = am.compute_metrics(frozen, "Go", verdicts_at(mixed), {p: "" for p in programs})
    check(abs(metrics["Early_Error_Detection"]["score"] - 37.5 / 26) < 1e-6, metrics["Early_Error_Detection"])
    # Type Safety on that state: ADV-01 hazard stage mean 37.5, static 0 -> 0.5*(37.5/6).
    check(abs(metrics["Type_Safety"]["score"] - 0.5 * (37.5 / 6)) < 1e-6, metrics["Type_Safety"])

    # Memory Safety is an integrity indicator: a diagnosed runtime trap counts as
    # fully preserved, and so does a compile-time rejection.
    ms = {p: am.SB for p in programs}
    ms["ADV-09/C"] = am.CTD
    ms["ADV-09/R"] = am.RSD
    ms["ADV-19"] = am.CRASH
    metrics = am.compute_metrics(frozen, "Zig", verdicts_at(ms), {p: "" for p in programs})
    check(abs(metrics["Memory_Safety"]["score"] - 100.0 / 9) < 1e-6, metrics["Memory_Safety"])

    # Implementation Robustness counts toolchain flags only.
    ir = verdicts_at({p: am.CRASH for p in programs})
    ir["ADV-21"]["flags"] = ["toolchain_crash"]
    metrics = am.compute_metrics(frozen, "Swift", ir, {p: "" for p in programs})
    check(abs(metrics["Implementation_Robustness"]["score"] - 100.0 * (1 - 1 / 34)) < 1e-6, metrics["Implementation_Robustness"])

    # Debuggability reads the fault report, not the rung: same rung, different reports.
    dbg = verdicts_at({p: am.RSD for p in programs})
    reports = {p: "" for p in programs}
    reports["ADV-08/R"] = "thread 'main' panicked at src/main.rs:9:5:\nattempt to divide by zero\nerror[E0000]"
    metrics = am.compute_metrics(frozen, "Rust", dbg, reports)
    check(abs(metrics["Debuggability"]["score"] - (100.0 / 2) / 26) < 1e-6, metrics["Debuggability"])

    # N/A rows leave numerator and denominator alone.
    partial = verdicts_at({p: am.CTD for p in programs})
    partial["ADV-23"] = {"earliest_observable_stage": None, "stage_score": None, "flags": ["na"], "na": {"reason": "host"}}
    metrics = am.compute_metrics(frozen, "Java", partial, {p: "x.java:1: error[E1]" for p in programs})
    check(metrics["Early_Error_Detection"]["score"] == 100.0 and metrics["Early_Error_Detection"]["n_cases_na"] == 1, metrics["Early_Error_Detection"])


def test_program_resolution_covers_every_template_spelling(frozen) -> None:
    programs = [p for row in frozen.rows for p in row["programs"]]
    missing: dict[str, list[str]] = {}
    for language in am.COMPARISON_LANGUAGES:
        slug = am.LANGUAGE_IDS[language]
        directory = TEMPLATE / "programs" / slug / "adversarial"
        for program in programs + ["ADV-22-valid", "ADV-21_depth1000", "ADV-21_depth10000"]:
            determination = frozen.tm3_for(language, program, None)
            if determination and determination.get("branch") == "TM3a":
                # No program exists for a construct the language does not have.
                continue
            if am.resolve_source(language, directory, program) is None:
                missing.setdefault(language, []).append(program)
    check(not missing, f"template adversarial programs unresolved: {missing}")
    for language in am.COMPARISON_LANGUAGES:
        slug = am.LANGUAGE_IDS[language]
        directory = TEMPLATE / "programs" / slug / "adversarial"
        source = am.resolve_source(language, directory, "ADV-01/R")
        span = am.construction_line_range(source)
        check(span is not None and span["first_line"] <= span["last_line"], f"{language}: no construction span in {source}")


def test_inputs_are_generated_from_the_case_set(frozen) -> None:
    with tempfile.TemporaryDirectory() as td:
        work = Path(td) / "cell"
        work.mkdir()
        case = frozen.cases["ADV-23"]
        stdin = am.write_inputs(frozen, work, Path(td) / "nolang", case, None)
        data = (work / "inputs" / "ADV-23.bin").read_bytes()
        check(stdin is None and data.hex() == "41c328eda080f490808080420a", data.hex())
        case = frozen.cases["ADV-26"]
        stdin = am.write_inputs(frozen, work, Path(td) / "nolang", case, None)
        check(stdin is not None and stdin.read_text() == "5\n9\n2\n14\n3\n20\n8\n3\n", stdin)
        case, sub = frozen.case_for_program("ADV-04b/R")
        stdin = am.write_inputs(frozen, work, Path(td) / "nolang", case, sub)
        check(stdin is not None and stdin.name == "ADV-04b.in" and stdin.read_text() == "-1\n1\n", stdin)
        # The frozen per-language override for the unsigned index branch applies.
        lang_dir = Path(td) / "rustlike"
        (lang_dir / "inputs_lang").mkdir(parents=True)
        (lang_dir / "inputs_lang" / "ADV-09.in").write_text("0\n")
        case = frozen.cases["ADV-09"]
        stdin = am.write_inputs(frozen, work, lang_dir, case, None)
        check(stdin is not None and stdin.read_text() == "0\n", stdin)


def test_synthetic_measure_writes_a_complete_result() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit = {
            "id": "lq-adversarial-mechanical", "runner_action": "adversarial-measure",
            "requirement_ids": list(am.REQUIREMENT_METRICS),
        }
        (root / "work" / "root" / "manifest.json").write_text(json.dumps({"work_units": [unit]}))
        os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = "1"
        try:
            rc = am.measure(root, "lq-adversarial-mechanical")
        finally:
            os.environ.pop("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS", None)
        result = json.loads((root / "work/root/commands/lq-adversarial-mechanical/result.json").read_text())
        check(rc == 0 and set(result["requirements"]) == set(am.REQUIREMENT_METRICS), result)
        check(all(set(v) == set(am.LANGUAGES) for v in result["requirements"].values()), result)


def test_missing_toolchains_block_rather_than_score(frozen) -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit = {
            "id": "lq-adversarial-mechanical", "runner_action": "adversarial-measure",
            "requirement_ids": list(am.REQUIREMENT_METRICS),
        }
        (root / "work" / "root" / "manifest.json").write_text(json.dumps({"work_units": [unit]}))
        (root / "results" / "toolchains.json").write_text(json.dumps({"missing": ["Zig", "Swift"]}))
        try:
            am.measure(root, "lq-adversarial-mechanical")
        except am.MeasureError as exc:
            check("missing required comparison toolchains" in str(exc) and "Zig" in str(exc), str(exc))
        else:
            check(False, "a missing comparison toolchain did not stop the measurement")


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        frozen = am.Frozen(root)
        tests = [
            (test_lexicon_fragments_match_hazards_and_not_ordinary_text, (frozen,)),
            (test_decision_list_replays_each_rung, (frozen,)),
            (test_metrics_follow_the_frozen_formulas, (frozen,)),
            (test_program_resolution_covers_every_template_spelling, (frozen,)),
            (test_inputs_are_generated_from_the_case_set, (frozen,)),
            (test_synthetic_measure_writes_a_complete_result, ()),
            (test_missing_toolchains_block_rather_than_score, (frozen,)),
        ]
        for test, args in tests:
            try:
                test(*args)
            except Exception as exc:  # surface the failing test rather than aborting the file
                FAILURES.append(f"{test.__name__} raised {type(exc).__name__}: {exc}")
    if FAILURES:
        print("adversarial scorer failures:", file=sys.stderr)
        for failure in FAILURES:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"adversarial scorer checks passed ({len(tests)} groups)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
