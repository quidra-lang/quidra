#!/usr/bin/env python3
"""Regression tests for Semantic Compression reconciliation and comparability."""

from __future__ import annotations

import importlib.util
import json
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
                        "PARTIAL", "go_fragment", partial=["P-b"]
                    ),
                }],
            },
        }
        assert benchmark.persist_comparability_repairs(root, result) == 1
        assert benchmark.persist_comparability_repairs(root, result) == 0
        repaired = benchmark.sc_comparability_repairs(root)
        assert repaired["F20.P1"]["Go"]["partial_reasons"] == ["P-b"]

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
        assert (
            "template/config/semantic_compression_comparability.json"
            in unit.get("read_paths", [])
        ), probe
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
        report = benchmark.validate_canonical_fragment_verification(
            root, "Python", catalog, evidence
        )
        assert report["synthetic_ci"] is True, report
        assert "probes" not in report, report


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
        assert "do not contradict its build/run facts" in payload["task"], payload["task"]


def main() -> None:
    assert_complete_support_record_contract()
    assert_support_adjudication_receives_trusted_verification()
    assert_probe_alias_rows_are_recognized()
    assert_conflicting_annotation_fields_are_rejected()
    assert_adjudication_is_authoritative()
    assert_repair_loop_is_scoped_and_idempotent()
    assert_every_sampled_probe_has_a_cohort_adjudicator()
    assert_cohort_work_is_cacheable()
    assert_canonical_fragment_verification_runs_real_recipe()
    assert_synthetic_verification_cannot_masquerade_as_real()
    assert_go_multi_unit_recipe_builds_one_main_package()
    print("semantic compression reconciliation contract: ok")


if __name__ == "__main__":
    main()
