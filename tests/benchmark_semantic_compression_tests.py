#!/usr/bin/env python3
"""Regression tests for Semantic Compression reconciliation and comparability."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "benchmark_cli", ROOT / "benchmark/template/scripts/benchmark.py"
)
benchmark = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(benchmark)


def canonical(level: str, fragment: str | None, *, partial=None, none=None):
    return {
        "level": level,
        "fragment": fragment,
        "partial_reasons": list(partial or []),
        "none_reason": none,
        "justification": "frozen-rubric justification",
        "citation": "frozen evidence citation",
    }


def make_root(td: str) -> Path:
    root = Path(td)
    (root / "work/root").mkdir(parents=True)
    (root / "work/audit/semantic-compression").mkdir(parents=True)
    (root / "home").mkdir(parents=True)
    (root / "tmp").mkdir(parents=True)
    # The tests exercise the real frozen matrix/policy without copying it.
    (root / "template").symlink_to((ROOT / "benchmark/template").resolve(), target_is_directory=True)
    return root


def assert_complete_support_record_contract() -> None:
    assert benchmark.sc_adjudicated_record("FULL") is None
    assert benchmark.sc_adjudicated_record(
        canonical("FULL", "let x = 1")
    )["level"] == "FULL"
    assert benchmark.sc_adjudicated_record(
        canonical("PARTIAL", "ffi_call()", partial=["P-b"])
    )["partial_reasons"] == ["P-b"]
    assert benchmark.sc_adjudicated_record(
        canonical("PARTIAL", "ffi_call()")
    ) is None
    assert benchmark.sc_adjudicated_record(
        canonical("NONE", None, none="N-1")
    )["none_reason"] == "N-1"
    assert benchmark.sc_adjudicated_record(
        canonical("NONE", "should-not-exist", none="N-1")
    ) is None


def assert_probe_alias_rows_are_recognized() -> None:
    result = {
        "evidence": {
            "per_probe": [
                {"probe": "F08.P1", "support": "PARTIAL", "p_letters": ["P-a"]},
                {"probe_id": "F10.P1", "support": "FULL"},
            ]
        }
    }
    rows = benchmark.probe_annotation_fields(result, {"F08.P1", "F10.P1"})
    assert rows["F08.P1"]["support"] == "PARTIAL", rows
    assert rows["F08.P1"]["p_letters"] == ["P-a"], rows
    assert rows["F10.P1"]["support"] == "FULL", rows


def assert_conflicting_annotation_fields_are_rejected() -> None:
    target = {"fragment": "same", "metric_fact": 1}
    benchmark.merge_annotation_fields_strict(
        target,
        {"fragment": "same", "other_fact": 2},
        context="F20.P1/Python",
    )
    assert target["other_fact"] == 2

    try:
        benchmark.merge_annotation_fields_strict(
            target,
            {"fragment": "different"},
            context="F20.P1/Python",
        )
    except benchmark.BenchmarkError as exc:
        assert "conflicting annotation field" in str(exc)
    else:
        raise AssertionError("conflicting shard annotations were silently overwritten")

    result = {
        "evidence": {
            "a": [{"probe": "F20.P1", "fragment": "first"}],
            "b": [{"probe_id": "F20.P1", "fragment": "second"}],
        }
    }
    try:
        benchmark.probe_annotation_fields(result, {"F20.P1"})
    except benchmark.BenchmarkError as exc:
        assert "conflicting annotation field" in str(exc)
    else:
        raise AssertionError("probe collection silently stitched conflicting fields")

    combined: dict[str, object] = {}
    benchmark.sc_add_annotation_source(
        combined,
        "density-shard",
        {"fragment": "same", "note": "density explanation"},
    )
    benchmark.sc_add_annotation_source(
        combined,
        "determinacy-shard",
        {"fragment": "same", "note": "different but valid explanation"},
    )
    assert "fragment" not in combined and "note" not in combined, combined
    assert set(combined["metric_annotations"]) == {
        "density-shard", "determinacy-shard"
    }, combined
    assert (
        combined["metric_annotations"]["density-shard"]["note"]
        != combined["metric_annotations"]["determinacy-shard"]["note"]
    ), combined


def assert_metric_annotations_do_not_carry_support_authority() -> None:
    fields = {
        "fragment": "stale_fragment()",
        "support": "NONE",
        "support_factor": 0.0,
        "p_letters": ["P-b"],
        "none_reason": "N-3",
        "level": "HIGH",
        "note": "metric-specific explanation",
        "citation": "metric-specific citation",
        "semantic_fact_count": 4,
    }
    cleaned = benchmark.sc_metric_only_annotation(fields)
    assert "fragment" not in cleaned, cleaned
    assert "support" not in cleaned, cleaned
    assert "support_factor" not in cleaned, cleaned
    assert "p_letters" not in cleaned, cleaned
    assert "none_reason" not in cleaned, cleaned
    assert cleaned["level"] == "HIGH", cleaned
    assert cleaned["note"] == "metric-specific explanation", cleaned
    assert cleaned["citation"] == "metric-specific citation", cleaned
    assert cleaned["semantic_fact_count"] == 4, cleaned
    support_level = benchmark.sc_metric_only_annotation({"level": "FULL"})
    assert "level" not in support_level, support_level


def assert_f20_runtime_facts_contract() -> None:
    smoke = {
        language: {
            "probe_id": "F20.P1",
            "mechanism": f"{language} built-in mechanism",
            "frozen_recipe_extra_flags": [],
            "build": (
                None
                if language == "Python"
                else {"argv": ["compiler", "source"], "exit_code": 0}
            ),
            "run": {
                "argv": ["runtime", "program"],
                "exit_code": 0,
                "stdout": "3\n",
                "stderr": "",
            },
            "passed": True,
        }
        for language in benchmark.F20_RUNTIME_REQUIRED_LANGUAGES
    }
    stable = benchmark.normalize_f20_runtime_smoke(smoke)
    assert stable["probe_id"] == "F20.P1", stable
    assert set(stable["languages"]) == set(
        benchmark.F20_RUNTIME_REQUIRED_LANGUAGES
    ), stable
    assert all(
        row["observed_stdout"] == "3"
        and row["frozen_recipe_extra_flags"] == []
        for row in stable["languages"].values()
    ), stable

    bad = json.loads(json.dumps(smoke))
    bad["Go"]["frozen_recipe_extra_flags"] = ["CGO_ENABLED=1"]
    try:
        benchmark.normalize_f20_runtime_smoke(bad)
    except benchmark.BenchmarkError as exc:
        assert "extra flags" in str(exc)
    else:
        raise AssertionError("an F20 baseline with an extra flag was accepted")

    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        benchmark.json_dump(root / benchmark.F20_RUNTIME_FACTS_RELATIVE, stable)
        benchmark.validate_f20_record_against_runtime_baseline(
            root, "Go", canonical("FULL", "ffi_fragment()")
        )
        try:
            benchmark.validate_f20_record_against_runtime_baseline(
                root, "Go", canonical("NONE", None, none="N-1")
            )
        except benchmark.BenchmarkError as exc:
            assert "cannot be NONE" in str(exc)
        else:
            raise AssertionError("trusted F20 runtime evidence did not reject NONE")
        try:
            benchmark.validate_f20_record_against_runtime_baseline(
                root,
                "Go",
                canonical("PARTIAL", "ffi_fragment()", partial=["P-b"]),
            )
        except benchmark.BenchmarkError as exc:
            assert "cannot cite P-b" in str(exc)
        else:
            raise AssertionError("trusted F20 runtime evidence did not reject P-b")


def assert_f20_work_plan_uses_stable_runtime_facts() -> None:
    plan = json.loads(
        (ROOT / "benchmark/template/config/work_plan_templates.json").read_text()
    )
    units = {
        unit["id"]: unit
        for unit in plan["evaluations"]["semantic_compression"]["units"]
    }
    # Canonical authoring and the final comparability audit consume the
    # stable runner-owned facts directly. Cohort support adjudication receives
    # the same facts inside its dynamically generated support input, avoiding a
    # duplicate Task Packet dependency while still letting the current validator
    # enforce the runtime baseline.
    for unit_id in (
        "sc-metrics-hidden-coverage--part-2",
        "sc-comparability",
    ):
        reads = units[unit_id].get("read_paths", [])
        assert "work/root/f20_runtime_facts.json" in reads, (unit_id, reads)
        assert "results/toolchains.json" not in reads, (unit_id, reads)
        assert "template/runtime/f20_interop_fixtures.json" not in reads, (
            unit_id, reads
        )

    adjudication = units["sc-support-adjudication--f20-p1"]
    assert adjudication.get("read_paths", []) == [], adjudication.get("read_paths")
    assert "work/root/f20_runtime_facts.json" in adjudication.get("goal", "")


def assert_r9_p_a_scope_is_enforced() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        allowed = canonical("PARTIAL", "substitute()", partial=["P-a"])
        benchmark.validate_sc_record_for_probe(
            root, "F02.P2", allowed, context="allowed R9 substitution"
        )
        benchmark.validate_sc_record_for_probe(
            root, "F18.P2", allowed, context="allowed F18 private-boundary substitution"
        )
        assert "F18.P2" in benchmark.sc_p_a_allowed_probes(root)
        universe = benchmark.json_load(
            root
            / "template/methodology-assets/semantic_compression"
            / "capability_universe.json"
        )
        assert "F18.P2" in universe["authoring_rules_for_probe_fragments"]["R9_no_probe_substitution"]
        assert (
            "F18.P2"
            in universe["support_rubric"]["deterministic_tie_break"][
                "named_substitution"
            ]
        )

        forbidden = canonical("PARTIAL", "wrapper()", partial=["P-a"])
        try:
            benchmark.validate_sc_record_for_probe(
                root, "F10.P1", forbidden, context="forbidden R9 substitution"
            )
        except benchmark.BenchmarkError as exc:
            assert "cannot cite P-a" in str(exc), exc
        else:
            raise AssertionError("F10.P1 incorrectly accepted P-a outside R9 scope")


def assert_adjudication_is_authoritative() -> None:
    by_language = {
        "Go": {
            "F20.P1": {
                "support": "NONE",
                "p_letter": "",
                "justification": "stale single-language explanation",
                "citation": "stale citation",
                "fragment": "go_fragment",
                "metric_only_fact": 7,
            }
        }
    }
    owner_rows = {"Go": {"F20.P1": {"support": "NONE"}}}
    owner = {
        "fields": ["support"],
        "levels": {"FULL": 1.0, "PARTIAL": 0.5, "NONE": 0.0},
    }
    adjudicated = {
        "F20.P1": {
            "Go": canonical(
                "PARTIAL", "go_fragment", partial=["P-b"]
            )
        }
    }
    replaced = benchmark.sc_reconcile_support(
        by_language, owner_rows, owner, adjudicated
    )
    row = by_language["Go"]["F20.P1"]
    assert row["support"] == "PARTIAL", row
    assert row["support_factor"] == 0.5, row
    assert row["support_reason_codes"] == ["P-b"], row
    assert row["support_adjudication"]["partial_reasons"] == ["P-b"], row
    assert row["fragment"] == "go_fragment", row
    assert row["metric_only_fact"] == 7, row
    assert "p_letter" not in row, row
    assert row.get("justification") != "stale single-language explanation", row
    assert replaced and replaced[0]["authoritative_adjudication"]["level"] == "PARTIAL"


def assert_repair_loop_is_scoped_and_idempotent() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        benchmark.json_dump(
            root / benchmark.F20_RUNTIME_FACTS_RELATIVE,
            {
                "schema_version": 1,
                "probe_id": "F20.P1",
                "languages": {
                    "Go": {
                        "passed": True,
                        "frozen_recipe_extra_flags": [],
                        "observed_stdout": "3",
                    }
                },
            },
        )
        benchmark.json_dump(
            root / benchmark.COMPARABILITY_BLINDING_RELATIVE,
            {"schema_version": 1, "labels": {"Go": "A"}},
        )
        benchmark.json_dump(
            root / benchmark.COMPARABILITY_SAMPLE_RELATIVE,
            {
                "schema_version": 1,
                "probes": [{
                    "probe_id": "F20.P1",
                    "annotations": [{
                        "label": "A",
                        "support": "PARTIAL",
                        "fragment": "go_fragment",
                    }],
                }],
            },
        )
        result = {
            "schema_version": 1,
            "evaluation": "semantic_compression",
            "requirements": {benchmark.COMPARABILITY_GATE: False},
            "evidence": {
                "gate_result": {
                    "affected_pairs_requiring_revalidation": [
                        {"probe_id": "F20.P1", "label": "A"}
                    ]
                },
                "repair_directives": [{
                    "probe_id": "F20.P1",
                    "label": "A",
                    "record": canonical(
                        "PARTIAL", "go_fragment", partial=["P-c"]
                    ),
                }],
            },
        }
        assert benchmark.persist_comparability_repairs(root, result) == 1
        assert benchmark.persist_comparability_repairs(root, result) == 0
        repaired = benchmark.sc_comparability_repairs(root)
        assert repaired["F20.P1"]["Go"]["partial_reasons"] == ["P-c"]

        stale_flag_claim = json.loads(json.dumps(result))
        stale_flag_claim["evidence"]["repair_directives"][0]["record"] = canonical(
            "PARTIAL", "go_fragment", partial=["P-b"]
        )
        try:
            benchmark.persist_comparability_repairs(root, stale_flag_claim)
        except benchmark.BenchmarkError as exc:
            assert "cannot cite P-b" in str(exc), exc
        else:
            raise AssertionError(
                "F20.P1 repair must not contradict trusted no-extra-flag runtime facts"
            )

        unsafe = json.loads(json.dumps(result))
        unsafe["evidence"]["repair_directives"][0]["record"] = canonical(
            "NONE", None, none="N-1"
        )
        try:
            benchmark.validate_comparability_repair_directives(root, unsafe)
        except benchmark.BenchmarkError as exc:
            assert "NONE boundary" in str(exc)
        else:
            raise AssertionError("run-local repair must not cross the NONE boundary")

def assert_every_sampled_probe_has_a_cohort_adjudicator() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        sampled = {
            str(row["probe_id"]) for row in benchmark.comparability_sample_probes(root)
        }
    plan = json.loads(
        (ROOT / "benchmark/template/config/work_plan_templates.json").read_text()
    )
    units = plan["evaluations"]["semantic_compression"]["units"]
    supports = {
        benchmark.support_adjudication_probe(unit.get("requirement_ids", [])): unit
        for unit in units
        if str(unit.get("id", "")).startswith("sc-support-adjudication--")
    }
    assert set(supports) == sampled, (sorted(supports), sorted(sampled))
    for probe, unit in supports.items():
        assert probe in unit["goal"], (probe, unit["goal"])
        # The runner builds one dependency-ready support-adjudication input that
        # embeds the frozen comparability policy together with annotations,
        # mechanical verification and toolchain/runtime facts. Requiring the
        # same policy again as a static read path would duplicate Task Packet
        # input and needlessly perturb paid prompt/cache fingerprints.
        assert "comparability policy" in unit["goal"].lower(), probe
    comparability = next(unit for unit in units if unit["id"] == "sc-comparability")
    expected_dependencies = {
        "sc-support-adjudication--" + probe.lower().replace(".", "-")
        for probe in sampled
    }
    assert expected_dependencies <= set(comparability["dependencies"])


def assert_cohort_work_is_cacheable() -> None:
    support = {
        "id": "sc-support-adjudication--f20-p1",
        "execution_kind": "agent",
        "result_kind": "requirements",
        "phase": "measurement",
        "requirement_ids": ["annotation.support_adjudication--f20-p1"],
        "assigned_languages": [],
    }
    audit = {
        "id": "sc-comparability",
        "execution_kind": "agent",
        "result_kind": "requirements",
        "phase": "measurement",
        "requirement_ids": [benchmark.COMPARABILITY_GATE],
        "assigned_languages": [],
    }
    assert benchmark.cache_eligible_unit(ROOT, support)
    assert benchmark.cache_scope(support) == "cohort-f20-p1"
    assert benchmark.cache_eligible_unit(ROOT, audit)
    assert benchmark.cache_scope(audit) == "comparability"



def assert_canonical_fragment_verification_runs_real_recipe() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        matrix = json.loads(
            (
                ROOT
                / "benchmark/template/methodology-assets/semantic_compression/semantic_site_matrix.json"
            ).read_text()
        )
        catalog = {
            str(probe["probe_id"]): canonical("NONE", None, none="N-1")
            for probe in matrix["probes"]
        }
        fragment = "n = 7\nreturn n"
        catalog["F01.P1"] = canonical("FULL", fragment)
        evidence = {
            "canonical_verification": {
                "F01.P1": {
                    "entry_file": "main.py",
                    "fragment_files": ["main.py"],
                    "files": {
                        "main.py": (
                            "def probe():\n"
                            "    n = 7\n"
                            "    return n\n\n"
                            "assert probe() == 7\n"
                            "print(probe())\n"
                        )
                    },
                    "mode": "run",
                    "run_count": 1,
                }
            }
        }
        report = benchmark.validate_canonical_fragment_verification(
            root, "Python", catalog, evidence
        )
        assert report["verified_probe_count"] == 1, report
        assert report["probes"]["F01.P1"]["runs"][0]["exit_code"] == 0, report
        assert report["probes"]["F01.P1"]["expected_stdout"] == "7", report
        assert report["probes"]["F01.P1"]["runs"][0]["stdout"].strip() == "7", report

        wrong_stdout = json.loads(json.dumps(evidence))
        wrong_stdout["canonical_verification"]["F01.P1"]["files"]["main.py"] = (
            wrong_stdout["canonical_verification"]["F01.P1"]["files"]["main.py"]
            .replace("print(probe())", "print(8)")
        )
        try:
            benchmark.validate_canonical_fragment_verification(
                root, "Python", catalog, wrong_stdout
            )
        except benchmark.BenchmarkError as exc:
            assert "observed stdout mismatch" in str(exc), exc
        else:
            raise AssertionError(
                "a successful fixture with the wrong canonical result was accepted"
            )

        audit = root / "work/audit/semantic-compression/canonical_verification_python.json"
        assert audit.is_file(), audit

        broken = json.loads(json.dumps(evidence))
        broken["canonical_verification"]["F01.P1"]["files"]["main.py"] += (
            "\nraise SystemExit(7)\n"
        )
        try:
            benchmark.validate_canonical_fragment_verification(
                root, "Python", catalog, broken
            )
        except benchmark.BenchmarkError as exc:
            assert "frozen run recipe failed" in str(exc), exc
        else:
            raise AssertionError("a canonical fragment whose fixture fails must be rejected")


def assert_verification_fragment_files_participate_in_frozen_recipe() -> None:
    catalog = {
        "F01.P1": canonical("FULL", "n = 7\nreturn n"),
    }
    hidden = {
        "canonical_verification": {
            "F01.P1": {
                "entry_file": "main.py",
                "fragment_files": ["unused.py"],
                "files": {
                    "main.py": "print(7)\n",
                    "unused.py": "n = 7\nreturn n\n",
                },
                "mode": "run",
                "run_count": 1,
            }
        }
    }
    try:
        benchmark._semantic_verification_schema(catalog, hidden)
    except benchmark.BenchmarkError as exc:
        assert "single-unit verification fragment_files must be exactly" in str(exc), exc
    else:
        raise AssertionError(
            "single-unit canonical fragment hidden in an unbuilt file was accepted"
        )

    bad_multi = {
        "entry_file": "main.py",
        "fragment_files": ["main.py", "notes.txt"],
        "files": {
            "main.py": "import util\n",
            "notes.txt": "measured fragment\n",
            "util.py": "def pub_add(a, b): return a + b\n",
        },
        "mode": "run",
        "run_count": 1,
    }
    try:
        benchmark._semantic_validate_real_fragment_files(
            "Python", "F18.P2", bad_multi
        )
    except benchmark.BenchmarkError as exc:
        assert "non-source/unbuilt fixture files" in str(exc), exc
    else:
        raise AssertionError(
            "multi-unit canonical fragment hidden in a non-source file was accepted"
        )

    good_multi = dict(bad_multi)
    good_multi["fragment_files"] = ["main.py", "util.py"]
    benchmark._semantic_validate_real_fragment_files(
        "Python", "F18.P2", good_multi
    )


def assert_synthetic_verification_cannot_masquerade_as_real() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        matrix = json.loads(
            (
                ROOT
                / "benchmark/template/methodology-assets/semantic_compression/semantic_site_matrix.json"
            ).read_text()
        )
        catalog = {
            str(probe["probe_id"]): canonical("NONE", None, none="N-1")
            for probe in matrix["probes"]
        }
        catalog["F01.P1"] = canonical("FULL", "synthetic_fragment()")
        evidence = {
            "synthetic": "deterministic harness evidence",
            "canonical_verification": {
                "F01.P1": {
                    "entry_file": "main.txt",
                    "fragment_files": ["main.txt"],
                    "files": {"main.txt": "synthetic_fragment()\n"},
                    "mode": "run",
                    "run_count": 1,
                }
            },
        }
        try:
            benchmark.validate_canonical_fragment_verification(
                root, "Python", catalog, evidence
            )
        except benchmark.BenchmarkError as exc:
            assert "explicit non-canonical CI workspace" in str(exc), exc
        else:
            raise AssertionError(
                "model-authored synthetic evidence bypassed real verification"
            )

        previous = os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS")
        os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = "1"
        try:
            report = benchmark.validate_canonical_fragment_verification(
                root, "Python", catalog, evidence
            )
        finally:
            if previous is None:
                os.environ.pop("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS", None)
            else:
                os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = previous
        assert report["synthetic_ci"] is True, report
        assert "probes" not in report, report


def _write_semantic_owner_cohort(
    root: Path, overrides: dict[tuple[str, str], dict] | None = None
) -> tuple[list[str], list[str]]:
    overrides = overrides or {}
    languages = benchmark.metadata_languages(root)
    matrix = json.loads(
        (
            ROOT
            / "benchmark/template/methodology-assets/semantic_compression/semantic_site_matrix.json"
        ).read_text()
    )
    probe_ids = [str(probe["probe_id"]) for probe in matrix["probes"]]
    units = []
    for language in languages:
        agent_id = "owner-" + benchmark.slug_id(language)
        agent_dir = root / "work/agents" / agent_id
        agent_dir.mkdir(parents=True, exist_ok=True)
        catalog = {
            probe_id: overrides.get(
                (language, probe_id), canonical("FULL", "verified_fragment()")
            )
            for probe_id in probe_ids
        }
        (agent_dir / "result.json").write_text(
            json.dumps({
                "schema_version": 1,
                "evaluation": "semantic_compression",
                "requirements": {"metric.capability_coverage": {language: 100.0}},
                "evidence": {"canonical_fragments": catalog},
            }, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        verified = {}
        stdout_oracles = benchmark.semantic_fixed_stdout_oracles(root)
        for probe_id, record in catalog.items():
            if str(record["level"]).upper() == "NONE":
                continue
            if probe_id == "F20.P2":
                verified[probe_id] = {
                    "mode": "nm-add2",
                    "run_count": 0,
                    "canonical_fragment_sha256": benchmark.sha256_bytes(
                        str(record["fragment"]).encode("utf-8")
                    ),
                    "build": (
                        None
                        if language == "Python"
                        else {"argv": ["compiler", "source"], "exit_code": 0}
                    ),
                    "nm": {"argv": ["nm", "program"], "exit_code": 0},
                    "symbol_add2_defined": True,
                }
            else:
                run_count = 20 if probe_id == "F19.P2" else 1
                expected_stdout = stdout_oracles.get(probe_id)
                verified[probe_id] = {
                    "mode": "run",
                    "run_count": run_count,
                    "canonical_fragment_sha256": benchmark.sha256_bytes(
                        str(record["fragment"]).encode("utf-8")
                    ),
                    "build": (
                        None
                        if language == "Python"
                        else {"argv": ["compiler", "source"], "exit_code": 0}
                    ),
                    **(
                        {"expected_stdout": expected_stdout}
                        if expected_stdout is not None
                        else {}
                    ),
                    "runs": [
                        {
                            "argv": ["runtime", "program"],
                            "exit_code": 0,
                            **(
                                {"stdout": expected_stdout + "\n"}
                                if expected_stdout is not None
                                else {}
                            ),
                        }
                        for _ in range(run_count)
                    ],
                }
        benchmark.json_dump(
            root
            / "work/audit/semantic-compression"
            / f"canonical_verification_{benchmark.slug_id(language)}.json",
            {
                "schema_version": 1,
                "language": language,
                "synthetic_ci": False,
                "probes": verified,
            },
        )
        units.append({
            "id": "owner--" + benchmark.slug_id(language),
            "evaluation": "semantic_compression",
            "canonical_fragment_owner": True,
            "assigned_languages": [language],
            "assigned_agent_id": agent_id,
            "requirement_ids": ["metric.capability_coverage"],
        })
    (root / "work/root/manifest.json").write_text(
        json.dumps({"schema_version": 1, "work_units": units}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return languages, probe_ids


def assert_premeasurement_cohort_gate() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        languages, probe_ids = _write_semantic_owner_cohort(root)
        summary = benchmark.semantic_premeasurement_cohort_summary(root)
        assert summary["passed"] is True, summary
        assert not summary["v3_probes_without_full"], summary
        assert not summary["v4_languages_over_one_third_none"], summary
        assert set(summary["v1_mechanical_verification"]) == set(languages), summary
        assert all(
            row["verified_probe_count"] == len(probe_ids)
            for row in summary["v1_mechanical_verification"].values()
        ), summary

        missing_report = (
            root
            / "work/audit/semantic-compression"
            / f"canonical_verification_{benchmark.slug_id(languages[0])}.json"
        )
        missing_report.unlink()
        try:
            benchmark.semantic_premeasurement_cohort_summary(root)
        except benchmark.BenchmarkError as exc:
            assert "mechanical verification report is missing" in str(exc), exc
        else:
            raise AssertionError("premeasurement gate accepted a missing V1 report")
        _write_semantic_owner_cohort(root)

        stale = json.loads(missing_report.read_text())
        first_probe = next(iter(stale["probes"]))
        stale["probes"][first_probe]["canonical_fragment_sha256"] = "0" * 64
        missing_report.write_text(
            json.dumps(stale, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        try:
            benchmark.semantic_premeasurement_cohort_summary(root)
        except benchmark.BenchmarkError as exc:
            assert "canonical fragment hash mismatch" in str(exc), exc
        else:
            raise AssertionError("premeasurement gate accepted a stale V1 report")
        _write_semantic_owner_cohort(root)

        target_probe = probe_ids[0]
        overrides = {
            (language, target_probe): canonical(
                "PARTIAL", "verified_fragment()", partial=["P-c"]
            )
            for language in languages
        }
        _write_semantic_owner_cohort(root, overrides)
        summary = benchmark.semantic_premeasurement_cohort_summary(root)
        assert summary["passed"] is False, summary
        assert target_probe in summary["v3_probes_without_full"], summary

        suspicious_language = languages[0]
        overrides = {
            (suspicious_language, probe_id): canonical("NONE", None, none="N-1")
            for probe_id in probe_ids[:15]
        }
        _write_semantic_owner_cohort(root, overrides)
        summary = benchmark.semantic_premeasurement_cohort_summary(root)
        assert summary["passed"] is True, summary
        assert summary["v4_languages_over_one_third_none"][suspicious_language] == 15, summary
        assert not summary["v3_probes_without_full"], summary
        investigation = summary["v4_investigation"][suspicious_language]
        assert investigation["record_contract_review"] == "PASS", investigation
        assert investigation["cohort_expressibility_review"] == "PASS", investigation
        assert investigation["none_count"] == 15, investigation


def assert_v3_survives_support_adjudication() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        languages, probe_ids = _write_semantic_owner_cohort(root)
        probe_id = probe_ids[0]
        value = {
            # Use a probe-valid PARTIAL reason so this test reaches the V3
            # cohort invariant it is meant to exercise. P-a is intentionally
            # restricted by R9 and F01.P1 is outside that allowlist.
            language: canonical(
                "PARTIAL", "verified_fragment()", partial=["P-c"]
            )
            for language in languages
        }
        try:
            benchmark.validate_support_adjudication_against_canonical_fragments(
                root,
                "annotation.support_adjudication--" + probe_id.lower().replace(".", "-"),
                value,
            )
        except benchmark.BenchmarkError as exc:
            assert "pre-measurement V3" in str(exc), exc
        else:
            raise AssertionError(
                "support adjudication removed the cohort's last FULL implementation"
            )


def assert_v3_survives_comparability_repair() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        languages = benchmark.metadata_languages(root)
        labels = {
            language: chr(ord("A") + index)
            for index, language in enumerate(languages)
        }
        benchmark.json_dump(
            root / benchmark.COMPARABILITY_BLINDING_RELATIVE,
            {"schema_version": 1, "labels": labels},
        )
        probe_id = "F01.P1"
        annotations = []
        for index, language in enumerate(languages):
            annotations.append({
                "label": labels[language],
                "support": "FULL" if index == 0 else "PARTIAL",
                "fragment": "verified_fragment()",
            })
        benchmark.json_dump(
            root / benchmark.COMPARABILITY_SAMPLE_RELATIVE,
            {
                "schema_version": 1,
                "probes": [{
                    "probe_id": probe_id,
                    "annotations": annotations,
                }],
            },
        )
        first_label = labels[languages[0]]
        result = {
            "schema_version": 1,
            "evaluation": "semantic_compression",
            "requirements": {benchmark.COMPARABILITY_GATE: False},
            "evidence": {
                "gate_result": {
                    "affected_pairs_requiring_revalidation": [
                        {"probe_id": probe_id, "label": first_label}
                    ]
                },
                "repair_directives": [{
                    "probe_id": probe_id,
                    "label": first_label,
                    "record": canonical(
                        "PARTIAL", "verified_fragment()", partial=["P-c"]
                    ),
                }],
            },
        }
        try:
            benchmark.validate_comparability_repair_directives(root, result)
        except benchmark.BenchmarkError as exc:
            assert "pre-measurement V3" in str(exc), exc
        else:
            raise AssertionError(
                "comparability repair removed the cohort's last FULL implementation"
            )


def assert_capability_efficiency_uses_final_support_denominator() -> None:
    aggregation = json.loads(
        (ROOT / "benchmark/template/config/aggregation.json").read_text()
    )["evaluations"]["semantic_compression"]
    rule = aggregation["recompute_from_evidence"]["metrics"][
        "metric.capability_efficiency"
    ]
    evidence = {
        "raw_efficiency": 99.0,
        "total_semantic_complexity_units": 44.0,
        "supported_capability_points": 88.0,
    }
    assert benchmark.sc_language_ratio(evidence, rule) == 99.0
    assert benchmark.sc_language_ratio(
        evidence, rule, denominator_override=44.0
    ) == 1.0

    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        languages = benchmark.metadata_languages(root)
        matrix = json.loads(
            (
                ROOT
                / "benchmark/template/methodology-assets/semantic_compression/semantic_site_matrix.json"
            ).read_text()
        )
        probe_id = "F02.P2"
        assert probe_id in {str(probe["probe_id"]) for probe in matrix["probes"]}
        partial_language = languages[0]
        _write_semantic_owner_cohort(
            root,
            {
                (partial_language, probe_id): canonical(
                    "PARTIAL", "verified_fragment()", partial=["P-a"]
                )
            },
        )
        settled = benchmark.sc_supported_capability_points(
            root, aggregation, languages
        )
        assert settled is not None
        points, total = settled
        assert total == 88.0, settled
        assert points[partial_language] == 87.0, settled
        for language in languages[1:]:
            assert points[language] == 88.0, (language, settled)


def assert_premeasurement_gate_is_wired() -> None:
    plan = json.loads(
        (ROOT / "benchmark/template/config/work_plan_templates.json").read_text()
    )
    requirements = json.loads(
        (ROOT / "benchmark/template/config/evaluation_requirements.json").read_text()
    )
    required = requirements["evaluations"]["semantic_compression"]["required"]
    assert "gate.semantic_premeasurement_validation" in required, required
    units = plan["evaluations"]["semantic_compression"]["units"]
    gate = next(unit for unit in units if unit["id"] == "sc-premeasurement-validation")
    assert gate["runner_action"] == "semantic-premeasurement-validation", gate
    assert gate["dependencies"] == ["sc-metrics-hidden-coverage--part-2"], gate
    consumers = [
        unit for unit in units
        if unit.get("canonical_fragment_source_requirement") == "metric.capability_coverage"
    ]
    assert len(consumers) == 5, [unit["id"] for unit in consumers]
    for unit in consumers:
        assert "sc-metrics-hidden-coverage--part-2" in unit["dependencies"], unit
        assert "sc-premeasurement-validation" in unit["dependencies"], unit


def assert_semantic_recipe_runner_matches_frozen_contract() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        problems = benchmark.semantic_verification_recipe_drift_problems(root)
        assert problems == [], problems

        original = benchmark._semantic_verification_recipe

        def drifted(language, probe_id, entry, files):
            build, run, artifact = original(language, probe_id, entry, files)
            if language == "Go" and probe_id == "F01.P1":
                build = list(build or []) + ["--drift"]
            return build, run, artifact

        benchmark._semantic_verification_recipe = drifted
        try:
            problems = benchmark.semantic_verification_recipe_drift_problems(root)
        finally:
            benchmark._semantic_verification_recipe = original
        assert any("Go: single-unit build recipe drift" in row for row in problems), problems


def assert_go_multi_unit_recipe_builds_one_main_package() -> None:
    build, run, artifact = benchmark._semantic_verification_recipe(
        "Go",
        "F18.P2",
        "main.go",
        {
            "main.go": "package main\n",
            "go.mod": "module example\n",
            "util/util.go": "package util\n",
        },
    )
    assert build == ["go", "build", "-o", "program", "."], build
    assert run == ["./program"], run
    assert artifact == "program", artifact


def assert_support_adjudication_receives_trusted_verification() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        agent_dir = root / "work/agents/worker-python"
        agent_dir.mkdir(parents=True)
        benchmark.json_dump(
            agent_dir / "result.json",
            {
                "requirements": {},
                "evidence": {
                    "probe_rows": [
                        {
                            "probe_id": "F20.P1",
                            "fragment": "ffi_fragment()",
                            "note": "candidate support annotation",
                        }
                    ]
                },
            },
        )
        benchmark.json_dump(
            root
            / "work/audit/semantic-compression/canonical_verification_python.json",
            {
                "schema_version": 1,
                "language": "Python",
                "synthetic_ci": False,
                "probes": {
                    "F20.P1": {
                        "mode": "run",
                        "run_count": 1,
                        "build": None,
                        "runs": [
                            {
                                "argv": ["python3", "main.py"],
                                "exit_code": 0,
                                "stdout": "3\n",
                                "stderr": "",
                            }
                        ],
                    }
                },
            },
        )
        (root / "results").mkdir(exist_ok=True)
        benchmark.json_dump(
            root / "results/toolchains.json",
            {
                "schema_version": 1,
                "toolchains": {
                    "Python": {
                        "canonical": "3.14.5",
                        "commands": [["python3", "--version"]],
                    }
                },
            },
        )
        benchmark.json_dump(
            root / benchmark.F20_RUNTIME_FACTS_RELATIVE,
            {
                "schema_version": 1,
                "probe_id": "F20.P1",
                "source": "/opt/quidra-benchmark/toolchains-observed.json",
                "languages": {
                    "Python": {
                        "mechanism": "standard-library ctypes",
                        "passed": True,
                        "frozen_recipe_extra_flags": [],
                        "build_argv": None,
                        "run_argv": ["python3", "ffi.py"],
                        "observed_stdout": "3",
                    }
                },
            },
        )
        source = {
            "id": "sc-source--python",
            "assigned_agent_id": "worker-python",
            "assigned_languages": ["Python"],
            "requirement_ids": ["metric.semantic_density"],
        }
        unit = {
            "id": "sc-support-adjudication--f20-p1",
            "dependencies": [source["id"]],
        }
        path = benchmark.build_support_adjudication_input(
            root,
            unit,
            {"work_units": [source, unit]},
            "F20.P1",
        )
        payload = benchmark.json_load(path)
        verified = payload["mechanical_verification"]["Python"]
        assert verified["verified"] is True, verified
        assert verified["runs"][0]["argv"] == ["python3", "main.py"], verified
        assert verified["runs"][0]["exit_code"] == 0, verified
        assert verified["runs"][0]["stdout"] == "3\n", verified
        assert payload["frozen_toolchains"]["Python"]["canonical"] == "3.14.5", payload
        assert payload["trusted_runtime_baselines"]["languages"]["Python"][
            "observed_stdout"
        ] == "3", payload
        contract = payload["frozen_support_contract"]
        assert contract["authoring_rules"]["R9_no_probe_substitution"], contract
        assert contract["support_rubric"]["levels"]["PARTIAL"], contract
        assert contract["toolchain_binding"]["recipes"]["Python"]["run"] == "python3 FILE.py"
        sampled = {
            str(row["probe_id"]) for row in benchmark.comparability_sample_probes(root)
        }
        assert set(contract["sampled_probe_contracts"]) == sampled, contract
        assert "F20.P1" in contract["sampled_probe_contracts"], contract
        assert "do not contradict those build/run facts" in payload["task"], payload["task"]

        plan = json.loads(
            (ROOT / "benchmark/template/config/work_plan_templates.json").read_text()
        )
        adjudicators = [
            row
            for row in plan["evaluations"]["semantic_compression"]["units"]
            if str(row.get("id", "")).startswith("sc-support-adjudication--")
        ]
        assert len(adjudicators) == 20, len(adjudicators)
        assert all(row.get("read_paths") == [] for row in adjudicators), adjudicators


def assert_failed_comparability_quarantines_affected_probe() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_root(td)
        agent_dir = root / "work/agents/worker-sc-comparability"
        agent_dir.mkdir(parents=True)
        benchmark.json_dump(
            root / benchmark.COMPARABILITY_BLINDING_RELATIVE,
            {
                "schema_version": 1,
                "labels": {"Python": "A", "Go": "B"},
            },
        )
        benchmark.json_dump(
            agent_dir / "result.json",
            {
                "requirements": {"gate.comparability_audit": False},
                "evidence": {
                    "gate_result": {
                        "affected_pairs_requiring_revalidation": [
                            {"probe_id": "F20.P1", "label": "A"}
                        ]
                    }
                },
            },
        )
        manifest = {
            "work_units": [
                {
                    "id": "sc-comparability",
                    "assigned_agent_id": "worker-sc-comparability",
                    "requirement_ids": ["gate.comparability_audit"],
                }
            ]
        }
        assert benchmark.unresolved_semantic_comparability_probes(
            root, manifest
        ) == {"F20.P1"}
        pairs = benchmark.unresolved_semantic_comparability_pairs(root, manifest)
        assert pairs == {("F20.P1", "Python")}, pairs

        adjudicator = {
            "id": "sc-support-adjudication--f20-p1",
            "evaluation": "semantic_compression",
            "requirement_ids": ["annotation.support_adjudication--f20-p1"],
            "assigned_languages": [],
        }
        python_metric = {
            "id": "sc-metrics-local--part-1--python",
            "evaluation": "semantic_compression",
            "requirement_ids": ["metric.semantic_density"],
            "assigned_languages": ["Python"],
        }
        go_metric = {
            **python_metric,
            "id": "sc-metrics-local--part-1--go",
            "assigned_languages": ["Go"],
        }
        assert benchmark.semantic_cache_quarantine_reason(
            adjudicator, {"F20.P1"}, pairs
        )
        assert benchmark.semantic_cache_quarantine_reason(
            python_metric, {"F20.P1"}, pairs
        )
        assert benchmark.semantic_cache_quarantine_reason(
            go_metric, {"F20.P1"}, pairs
        ) is None

        # If the blinded label map is missing/corrupt, fail safe: preserve the
        # affected probe but quarantine every language rather than certifying
        # a suspect shard whose label can no longer be resolved.
        benchmark.json_dump(
            agent_dir / "result.json",
            {
                "requirements": {"gate.comparability_audit": False},
                "evidence": {
                    "gate_result": {
                        "affected_pairs_requiring_revalidation": [
                            {"probe_id": "F20.P1", "label": "Z"}
                        ]
                    }
                },
            },
        )
        fallback = benchmark.unresolved_semantic_comparability_pairs(root, manifest)
        assert {
            language
            for probe, language in fallback
            if probe == "F20.P1"
        } == set(benchmark.metadata_languages(root)), fallback


def main() -> None:
    assert_complete_support_record_contract()
    assert_r9_p_a_scope_is_enforced()
    assert_failed_comparability_quarantines_affected_probe()
    assert_support_adjudication_receives_trusted_verification()
    assert_probe_alias_rows_are_recognized()
    assert_conflicting_annotation_fields_are_rejected()
    assert_metric_annotations_do_not_carry_support_authority()
    assert_f20_runtime_facts_contract()
    assert_f20_work_plan_uses_stable_runtime_facts()
    assert_adjudication_is_authoritative()
    assert_repair_loop_is_scoped_and_idempotent()
    assert_every_sampled_probe_has_a_cohort_adjudicator()
    assert_cohort_work_is_cacheable()
    assert_canonical_fragment_verification_runs_real_recipe()
    assert_verification_fragment_files_participate_in_frozen_recipe()
    assert_synthetic_verification_cannot_masquerade_as_real()
    assert_premeasurement_cohort_gate()
    assert_v3_survives_support_adjudication()
    assert_v3_survives_comparability_repair()
    assert_capability_efficiency_uses_final_support_denominator()
    assert_premeasurement_gate_is_wired()
    assert_semantic_recipe_runner_matches_frozen_contract()
    assert_go_multi_unit_recipe_builds_one_main_package()
    print("semantic compression reconciliation contract: ok")


if __name__ == "__main__":
    main()
