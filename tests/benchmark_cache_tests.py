#!/usr/bin/env python3
"""Deterministic contract tests for the certified benchmark cache."""

from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "benchmark" / "template" / "scripts"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


benchmark = load("benchmark_cache_cli", SCRIPTS / "benchmark.py")
sys.modules["benchmark"] = benchmark
production = load("benchmark_cache_production", SCRIPTS / "production_run.py")


def make_workspace(tmp: Path) -> Path:
    root = tmp / "workspace"
    shutil.copytree(ROOT / "benchmark" / "template", root / "template")
    # init materializes the reuse catalog into the template before recording
    # its tree hash; readiness audits key on what it records.
    benchmark.materialize_reuse_catalog(ROOT, root / "template")
    (root / "cache").mkdir(parents=True)
    for rel in (
        "work/root",
        "work/agents",
        "work/attempts",
        "results",
        "raw",
        "prompts/by-hash",
        "prompts/components/by-hash",
        "prompts/manifests",
        "repo/docs",
        "home",
        "tmp",
        "gateway",
    ):
        (root / rel).mkdir(parents=True, exist_ok=True)

    master = (ROOT / "benchmark" / "master_prompt.md").read_bytes()
    master_hash = benchmark.sha256_bytes(master)
    (root / "prompts" / "by-hash" / f"{master_hash}.md").write_bytes(master)

    run = {
        "schema_version": 1,
        "run_id": "cache-contract",
        "workspace_root": "/quidra-benchmark",
        "sandbox_mode": "container",
        "master_prompt_sha256": master_hash,
        "primary_config_sha256": benchmark.sha256_file(
            root / "template" / "config" / "primary.json"
        ),
        "template_tree_sha256": benchmark.sha256_tree(root / "template"),
        "cache_tree_sha256": benchmark.sha256_tree(root / "cache"),
        "inference_identity": {
            "provider": "anthropic-messages",
            "model": "claude-sonnet-5",
        },
        "evaluated": {
            "commit_sha": "0" * 40,
            "compiler_version": "synthetic",
            "quidra_execution_identity": benchmark.json_load(
                root / "template/config/cache_policy.json"
            )["quidra_execution_identity"]["legacy_baseline"],
        },
        "created_at_utc": "2026-09-22T00:00:00+00:00",
    }
    benchmark.json_dump(root / "run.json", run)
    benchmark.json_dump(
        root / "results" / "toolchains.json",
        {
            "schema_version": 1,
            "toolchains": {
                "Python": {
                    "canonical": "3.14.5",
                    "raw": ["Python 3.14.5"],
                    "commands": [["python3", "--version"]],
                }
            },
            "missing": [],
            "ok": True,
        },
    )
    return root


def assert_language_scoped_program_reads(root: Path) -> None:
    reads = set(
        benchmark.planned_read_paths(
            root,
            ["repo/docs", "template/programs", "template/workloads"],
            ["Python"],
        )
    )
    expected = {
        str(root / "template/programs/python/micro"),
        str(root / "template/programs/python/adversarial"),
        str(root / "template/workloads"),
    }
    assert reads == expected, (reads, expected)
    assert not any("/programs/rust/" in path for path in reads)
    assert not any(path.endswith("/repo/docs") for path in reads)


def create_cacheable_task(root: Path) -> tuple[dict, dict]:
    unit_id = "cache-test--python"
    agent_id = "worker-cache-test--python"
    result_path = root / "work" / "agents" / agent_id / "result.json"
    validator = (
        f"python3 {root / 'template/scripts/benchmark.py'} result-check "
        f"--workspace {root} --id {agent_id}"
    )
    unit = {
        "id": unit_id,
        "evaluation": "ecosystem",
        "phase": "measurement",
        "execution_kind": "agent",
        "worker_mode": "sandbox-agent",
        "result_kind": "requirements",
        "goal": "Score one frozen ecosystem metric for Python.",
        "assigned_agent_id": agent_id,
        "assigned_languages": ["Python"],
        "dependencies": [],
        "input_hashes": {
            "primary_config": benchmark.primary_config_projection_sha256(
                root, "ecosystem"
            ),
            "benchmark_metadata": benchmark.sha256_file(
                root / "template/config/benchmark_metadata.json"
            ),
            "evaluation_spec_sections": benchmark.evaluation_spec_projection_sha256(
                root, "ecosystem", []
            ),
        },
        "reuse_audit_for": [],
        "requirement_ids": ["metric.documentation_quality"],
        "workload_ids": [],
        "read_paths": [str(root / "template/workloads/micro.md")],
        "evidence_paths": [str(result_path)],
        "validator_command": validator,
        "network_allowed": True,
        "prompt_sections": [],
        "max_attempts": 3,
        "max_llm_calls": 1,
        "estimated_input_tokens_per_call": 1000,
        "max_output_tokens_per_call": 1000,
    }

    benchmark.cmd_task_create(
        argparse.Namespace(
            workspace=str(root),
            id=agent_id,
            parent="RUNNER",
            evaluation="ecosystem",
            goal=unit["goal"],
            read=[str(root / "template/workloads/micro.md")],
            write=str(root / "work/agents" / agent_id),
            output=[str(result_path)],
            validate=validator,
            network=True,
            depth=1,
            section=[],
            requirement_id=unit["requirement_ids"],
            language=["Python"],
            worker_mode="sandbox-agent",
        )
    )
    task = benchmark.json_load(root / "work/agents" / agent_id / "task.json")
    return unit, task


def freeze_manifest(root: Path, unit: dict) -> None:
    manifest = {"schema_version": 1, "work_units": [unit]}
    benchmark.json_dump(root / "work/root/manifest.json", manifest)
    benchmark.json_dump(
        root / "work/root/ledger.json",
        {
            "schema_version": 1,
            "manifest_sha256": benchmark.sha256_file(
                root / "work/root/manifest.json"
            ),
            "units": {
                unit["id"]: {
                    "status": "PENDING",
                    "evidence_paths": unit["evidence_paths"],
                    "validation_result": None,
                    "blocker": None,
                    "blocker_class": None,
                    "attempts": 0,
                    "max_attempts": 3,
                    "attempt_history": [],
                    "updated_at_utc": "2026-09-22T00:00:00+00:00",
                    "heartbeat_at_utc": None,
                }
            },
        },
    )


def install_cache_record(root: Path, unit: dict, task: dict) -> str:
    pair = benchmark.cache_fingerprint(root, unit, task)
    assert pair is not None
    fingerprint, payload = pair
    rid = "metric.documentation_quality"
    ecosystem_asset = benchmark.ecosystem_rubric_asset(root)
    rubric = ecosystem_asset["metrics"][rid]
    component_ids = [row["id"] for row in rubric["components"]]
    result = {
        "schema_version": 1,
        "evaluation": "ecosystem",
        "requirements": {rid: {"Python": 75.0}},
        "evidence": {
            "certified": "synthetic cache contract",
            rid: {
                "rubric_id": rubric["rubric_id"],
                "component_levels": {cid: 3 for cid in component_ids},
                "component_findings": {
                    cid: f"synthetic cache evidence for {cid}"
                    for cid in component_ids
                },
                "sources": ["https://example.invalid/cache-evidence"],
                "candidate_universe": "synthetic cache candidate universe",
                "selection_rule": rubric["selection_rule"],
                "retrieval_route": "provider-brokered web search",
                "snapshot_date": ecosystem_asset["evidence_policy"]["snapshot_date"],
                "limitations": "",
            },
        },
    }
    result_raw = json.dumps(
        result, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    record = {
        "schema_version": 1,
        "fingerprint": fingerprint,
        "fingerprint_payload": payload,
        "evaluation": "ecosystem",
        "assigned_languages": ["Python"],
        "result": result,
        "result_sha256": benchmark.sha256_bytes(result_raw),
        "certification": {
            "validator_pass": True,
            "unit_complete": True,
            "primary_complete": False,
        },
        "provenance": {
            "run_id": "seed",
            "work_unit_id": unit["id"],
            "prompt_sha256": task["prompt_sha256"],
        },
    }
    rel = benchmark.cache_record_relative(unit, fingerprint)
    benchmark.json_dump(root / "cache" / rel, record)

    run = benchmark.json_load(root / "run.json")
    run["cache_tree_sha256"] = benchmark.sha256_tree(root / "cache")
    benchmark.json_dump(root / "run.json", run)
    return fingerprint


def assert_scored_cap_governs_reuse(root: Path, unit: dict, task: dict) -> None:
    """The cap a trial ran under decides reuse by evidence, not by the key.

    A trial that finished well inside a 4000-token cap is the same measurement
    under 16384, so raising the cap must not throw it away. A trial the cap cut
    off is a measurement of the cap, and is repeated. The key never contains
    the cap, so every record a cap-less unit already owns keeps its fingerprint.
    """
    pair = benchmark.cache_fingerprint(root, unit, task)
    assert pair is not None
    _, payload = pair
    assert "scored_output_cap" not in payload, payload
    raised = dict(unit, max_output_tokens_per_call=unit["max_output_tokens_per_call"] * 4)
    assert benchmark.cache_fingerprint(root, raised, task)[0] == pair[0], (
        "the scored cap must not be part of the cache key"
    )

    problem = benchmark.cache_cap_reuse_problem
    trial_unit = {**unit, "evaluation": "llm_learnability", "max_output_tokens_per_call": 16384}
    same_cap = {**trial_unit, "max_output_tokens_per_call": 4000}
    fitted = {"certification": {
        "scored_output_cap": 4000, "trial_calls": 6,
        "cap_truncated_trial_calls": 0, "max_trial_output_tokens": 188,
    }}
    cut_off = {"certification": {
        "scored_output_cap": 4000, "trial_calls": 6,
        "cap_truncated_trial_calls": 2, "max_trial_output_tokens": 4000,
    }}
    oversized = {"certification": {
        "scored_output_cap": 32768, "trial_calls": 6,
        "cap_truncated_trial_calls": 0, "max_trial_output_tokens": 20000,
    }}
    legacy = {"certification": {"learnability_integrity": True}}
    assert problem(root, fitted, trial_unit) is None, "a trial that never met its cap was refused"
    assert problem(root, cut_off, same_cap) is None, "the same cap is the same experiment"
    assert "cut off" in str(problem(root, cut_off, trial_unit)), "a truncated trial was reused"
    assert "above the current cap" in str(problem(root, oversized, trial_unit)), (
        "a completion larger than the current cap was reused"
    )
    assert "cache-annotate-caps" in str(problem(root, legacy, trial_unit)), (
        "a record without evidence was reused"
    )
    assert problem(root, legacy, unit) is None, "a packet-only record was held to trial evidence"
    assert problem(root, legacy, {**trial_unit, "max_output_tokens_per_call": 0}) is None

    # Annotation reads the run's retained traces and writes the evidence into
    # exactly the records that run promoted, without touching their results.
    evidence = root.parent / "evidence"
    agent_id = "worker-learnability-i1-i2--python"
    (evidence / "work" / "agents" / agent_id).mkdir(parents=True)
    (evidence / "work" / "root").mkdir(parents=True)
    benchmark.json_dump(evidence / "run.json", {"schema_version": 1, "run_id": "seed"})
    benchmark.json_dump(evidence / "work" / "root" / "manifest.json", {
        "schema_version": 1,
        "work_units": [{
            "id": "learnability-i1-i2--python", "evaluation": "llm_learnability",
            "assigned_agent_id": agent_id, "max_output_tokens_per_call": 4000,
        }],
    })
    benchmark.json_dump(evidence / "work" / "agents" / agent_id / "agent_trace.json", {
        "schema_version": 1,
        "trials": {"trials": {
            "i1-t1": {"calls": [
                {"stop_reason": "end_turn", "usage": {"output_tokens": 120}},
                {"stop_reason": "max_tokens", "usage": {"output_tokens": 4000}},
            ]},
            "i1-t2": {"calls": [{"stop_reason": "end_turn", "usage": {"output_tokens": 77}}]},
        }},
    })
    source = root.parent / "annotate-source"
    record_path = (
        source / "benchmark" / "cache" / "v1" / "llm-learnability" / "python" / ("a" * 64 + ".json")
    )
    record_path.parent.mkdir(parents=True)
    other_path = record_path.with_name("b" * 64 + ".json")
    record = {
        "schema_version": 1, "fingerprint": "a" * 64, "result": {"x": 1},
        "certification": {"learnability_integrity": True},
        "provenance": {"run_id": "seed", "work_unit_id": "learnability-i1-i2--python"},
    }
    benchmark.json_dump(record_path, record)
    benchmark.json_dump(other_path, {**record, "provenance": {"run_id": "another-run",
                                                             "work_unit_id": "learnability-i1-i2--python"}})
    summary = benchmark.annotate_cache_cap_evidence(source, evidence)
    assert [a["work_unit_id"] for a in summary["annotated"]] == ["learnability-i1-i2--python"], summary
    annotated = benchmark.json_load(record_path)
    assert annotated["result"] == {"x": 1} and annotated["fingerprint"] == "a" * 64
    certification = annotated["certification"]
    assert certification["scored_output_cap"] == 4000
    assert certification["trial_calls"] == 3
    assert certification["cap_truncated_trial_calls"] == 1
    assert certification["max_trial_output_tokens"] == 4000
    assert certification["learnability_integrity"] is True
    assert "certification" in benchmark.json_load(other_path)
    assert "scored_output_cap" not in benchmark.json_load(other_path)["certification"], (
        "a record from another run was annotated"
    )
    again = benchmark.annotate_cache_cap_evidence(source, evidence)
    assert again["annotated"] == [] and again["skipped"][0]["reason"] == "already annotated"
    assert "cut off" in str(problem(root, annotated, trial_unit))


def assert_accepted_trial_start_marks_the_scored_boundary() -> None:
    """A trial_start the runtime refused is not the first scored trial.

    The third paid run's promotion rejected three learnability units whose
    first trial_start was denied for missing attestations; the agent wrote
    them next and started again, exactly as the runtime demands.
    """
    denied = {"action": "trial_start", "turn": 7,
              "observation": {"ok": False, "denied": "Learnability scored trials are locked until ..."}}
    write = {"action": "write_file", "turn": 10, "observation": {"ok": True, "path": "x/learnability_preflight.json"}}
    accepted = {"action": "trial_start", "turn": 12, "observation": {"ok": True, "trials": []}}
    failed_trial = {"action": "trial_start", "turn": 12,
                    "observation": {"ok": False, "incomplete": "empty", "trial_id": "i1-t1"}}
    assert benchmark.first_accepted_trial_index([denied, write, accepted]) == 2
    assert benchmark.first_accepted_trial_index([denied, write, failed_trial]) == 2, (
        "a trial that ran but failed is still a scored start"
    )
    assert benchmark.first_accepted_trial_index([denied, write]) is None
    assert benchmark.first_accepted_trial_index([]) is None


def assert_readiness_audits_are_cacheable(root: Path, task: dict) -> None:
    """A reuse audit's verdict is keyed by the artifact object and its toolchain.

    Left out of the cache, the four Python and C++ audits were paid for by
    every run. The key carries the audited artifact's git object, the
    toolchain and pins of the artifact's language, and names the artifact in
    its scope, so a changed artifact or a bumped toolchain misses and nothing
    else does.
    """
    audit = {
        "id": "audit-micro-python", "evaluation": "language_quality", "phase": "readiness",
        "execution_kind": "agent", "worker_mode": "packet-only", "result_kind": "audit",
        "assigned_languages": [], "reuse_audit_for": ["micro-python"], "requirement_ids": [],
        "input_hashes": {"artifact_git_object": "a" * 40},
        "validator_command": "true", "network_allowed": True, "assigned_agent_id": "worker-audit-micro-python",
        "dependencies": [],
    }
    assert benchmark.cache_eligible_unit(root, audit), "a readiness audit is not cacheable"
    assert benchmark.audit_languages(root, audit) == ["Python"]
    pair = benchmark.cache_fingerprint(root, audit, task)
    assert pair is not None, "an audit produced no cache key"
    fingerprint, payload = pair
    assert payload["toolchains"] == {"Python": "3.14.5"} and payload["result_kind"] == "audit"
    assert payload["reuse_audit_for"] == ["micro-python"]
    assert benchmark.cache_scope(audit) == "audit-micro-python"
    # Semantic Compression records carry the evaluation's frozen depth; every
    # other evaluation's frozen_sampling is the run-wide declaration unchanged.
    sc_unit = dict(audit, id="sc-probe--python", evaluation="semantic_compression",
                   phase="measurement", result_kind="requirements", assigned_languages=["Python"],
                   reuse_audit_for=[], requirement_ids=["metric.semantic_density"])
    sc_payload = benchmark.cache_fingerprint(root, sc_unit, task)[1]
    assert sc_payload["frozen_sampling"]["effort"] == "medium", sc_payload["frozen_sampling"]
    assert sc_payload["frozen_sampling"]["effort_source"] == "evaluation_effort"
    assert sc_payload["cache_epoch"] == "2026-09-canonical-fragments-v6-fixed-stdout"
    assert payload["frozen_sampling"] == benchmark.sampling_config(root)
    assert "effort_source" not in payload["frozen_sampling"]
    changed = dict(audit, input_hashes={"artifact_git_object": "b" * 40})
    assert benchmark.cache_fingerprint(root, changed, task)[0] != fingerprint, (
        "a changed artifact must change the audit's key"
    )
    toolchains = benchmark.json_load(root / "results/toolchains.json")
    toolchains["toolchains"]["Python"]["canonical"] = "3.15.0"
    benchmark.json_dump(root / "results/toolchains.json", toolchains)
    assert benchmark.cache_fingerprint(root, audit, task)[0] != fingerprint, (
        "a bumped toolchain must change the audit's key"
    )
    toolchains["toolchains"]["Python"]["canonical"] = "3.14.5"
    benchmark.json_dump(root / "results/toolchains.json", toolchains)
    assert benchmark.cache_fingerprint(root, audit, task)[0] == fingerprint
    # Audits for a language absent from the toolchain scan have no key.
    orphan = dict(audit, reuse_audit_for=["micro-rust"])
    assert benchmark.cache_fingerprint(root, orphan, task) is None


def assert_mechanical_measurements_are_cacheable(root: Path, tmp: Path) -> None:
    """The micro suite, the adversarial set and the Quidra audit are certified.

    They are mechanical: no model, only the pinned image, the snapshot's
    compiler, the frozen programs and the measurement scripts. Left out of the
    cache, the third rehearsal spent five hours and fifty minutes of its
    six-hour job measuring them again and was cancelled before its one paid
    unit could finish. The key carries every measured language's toolchain
    and pins, the read paths, the measurement scripts, the Quidra versions and
    the declared mechanical epoch; the raw samples stay in the run artifact.
    """
    languages = list(benchmark.metadata_languages(root))
    toolchains = benchmark.json_load(root / "results/toolchains.json")
    for language in languages:
        if language == "Quidra":
            continue
        toolchains["toolchains"].setdefault(language, {"canonical": f"{language}-1.0-test"})
    benchmark.json_dump(root / "results/toolchains.json", toolchains)
    programs = root / "repo" / "tests" / "benchmark" / "quidra" / "micro"
    programs.mkdir(parents=True, exist_ok=True)
    (programs / "mb00.qui").write_text("print(1)\n", encoding="utf-8")
    (root / "repo" / "project.toml").write_text(
        'name = "Quidra"\nversion = "0.3.0"\nlanguage_version = "0.2"\n', encoding="utf-8"
    )
    result_path = root / "work" / "root" / "commands" / "lq-micro-mechanical" / "result.json"
    unit = {
        "id": "lq-micro-mechanical", "evaluation": "language_quality", "phase": "measurement",
        "execution_kind": "command", "result_kind": "requirements", "runner_action": "micro-measure",
        "requirement_ids": ["metric.native_execution_performance"], "dependencies": [],
        "read_paths": ["repo/tests/benchmark/quidra", "template/workloads"],
        "evidence_paths": [str(result_path)], "network_allowed": False, "max_attempts": 3,
        "worker_mode": "runner-command", "assigned_languages": [],
    }
    assert benchmark.cache_eligible_unit(root, unit), "a mechanical measurement is not cacheable"
    assert benchmark.cache_scope(unit) == "mechanical-micro-measure"
    task = benchmark.mechanical_task(unit)
    pair = benchmark.cache_fingerprint(root, unit, task)
    assert pair is not None, "a mechanical unit produced no cache key"
    fingerprint, payload = pair
    assert set(payload["toolchains"]) == set(languages) - {"Quidra"}, payload["toolchains"]
    assert payload["quidra_target"] == {"version": "0.3.0", "language_version": "0.2"}
    assert set(payload["measurement_script_hashes"]) == {"micro_measure.py", "adversarial_measure.py"}
    assert payload["provider"] is None and payload["model"] is None
    assert payload["result_kind"] == "mechanical" and payload["cache_epoch"] == "2026-09"
    assert "repo/tests/benchmark/quidra" in payload["readable_input_content_hashes"]

    # A completed measurement promotes into the source repository's cache.
    result = {
        "schema_version": 1, "evaluation": "language_quality",
        "requirements": {"metric.native_execution_performance": {lang: 50.0 for lang in languages}},
        "evidence": {"raw": "/quidra-benchmark/work/root/commands/lq-micro-mechanical/micro_raw.json"},
    }
    benchmark.json_dump(result_path, result)
    (result_path.parent / "micro_raw.json").write_text("{}\n", encoding="utf-8")
    freeze_manifest(root, unit)
    ledger = benchmark.json_load(root / "work/root/ledger.json")
    ledger["units"][unit["id"]].update({"status": "COMPLETE", "validation_result": "PASS"})
    benchmark.json_dump(root / "work/root/ledger.json", ledger)
    source = tmp / "source"
    (source / "benchmark" / "cache").mkdir(parents=True)
    summary = benchmark.promote_certified_cache(source, root)
    assert summary["promoted"] == 1, summary
    rel = benchmark.cache_record_relative(unit, fingerprint)
    record_path = source / "benchmark" / "cache" / rel
    assert record_path.is_file() and "mechanical-micro-measure" in rel.as_posix()
    record = benchmark.json_load(record_path)
    assert record["certification"]["mechanical"] is True
    assert (
        record["compatibility"]["quidra_execution_identity"]
        == benchmark.current_quidra_execution_identity(root)
    )
    assert "micro_raw.json" in record["certification"]["raw_evidence_sha256"]
    assert record["result"] == result

    # A verified legacy record keeps its historical fingerprint/result but is
    # ratcheted forward with the current execution identity at checkpoint.
    legacy_record = dict(record)
    legacy_record.pop("compatibility", None)
    benchmark.json_dump(record_path, legacy_record)
    upgraded = benchmark.promote_certified_cache(source, root)
    assert upgraded["promoted"] == 0 and upgraded["replaced"] == 0, upgraded
    assert upgraded["upgraded"] == 1, upgraded
    record = benchmark.json_load(record_path)
    assert (
        record["compatibility"]["quidra_execution_identity"]
        == benchmark.current_quidra_execution_identity(root)
    )
    assert benchmark.promote_certified_cache(source, root)["upgraded"] == 0

    # A later run with the same inputs hydrates the record instead of measuring.
    shutil.rmtree(result_path.parent)
    freeze_manifest(root, unit)
    benchmark.json_dump(root / "cache" / rel, record)
    run = benchmark.json_load(root / "run.json")
    run["cache_tree_sha256"] = benchmark.sha256_tree(root / "cache")
    benchmark.json_dump(root / "run.json", run)
    assert benchmark.hydrate_certified_cache(root, "language_quality") == 1
    ledger = benchmark.json_load(root / "work/root/ledger.json")
    assert ledger["units"][unit["id"]]["status"] == "COMPLETE"
    assert benchmark.json_load(result_path) == result
    status = benchmark.json_load(root / "results/cache_status.json")
    assert unit["id"] in status["hits"]

    # The runner reuses the record before its command could run: advance used
    # to execute every ready command unit first and consult the cache only
    # afterwards, so the certified five-hour micro suite was measured again
    # and the container smoke's re-measured audit collided with the certified
    # one at import. Here the measurement script cannot succeed (no compiler
    # in this workspace), so a unit that ran it would be retried and blocked.
    shutil.rmtree(result_path.parent)
    freeze_manifest(root, unit)
    benchmark.json_dump(root / "results" / "privacy_check.json", {"schema_version": 1, "ok": True})
    assert benchmark.cmd_advance(argparse.Namespace(
        workspace=str(root), evaluation="language_quality"
    )) == 0
    ledger = benchmark.json_load(root / "work/root/ledger.json")
    state = ledger["units"][unit["id"]]
    assert state["status"] == "COMPLETE" and state["validation_result"] == "PASS", state
    # One attempt: the hydration itself. A measurement attempt would have
    # failed and been retried, leaving FAIL entries behind.
    assert int(state.get("attempts", 0) or 0) == 1, state
    assert [a["result"] for a in state["attempt_history"]] == ["PASS"], state
    assert benchmark.json_load(result_path) == result
    assert (result_path.parent / "cache_receipt.json").is_file()
    status = benchmark.json_load(root / "results/cache_status.json")
    assert unit["id"] in status["hits"] and unit["id"] not in status["misses"]

    # A changed Quidra program or measurement script re-keys the measurement.
    (programs / "mb00.qui").write_text("print(2)\n", encoding="utf-8")
    assert benchmark.cache_fingerprint(root, unit, task)[0] != fingerprint
    (programs / "mb00.qui").write_text("print(1)\n", encoding="utf-8")
    script = root / "template" / "scripts" / "micro_measure.py"
    original = script.read_bytes()
    script.write_bytes(original + b"\n# changed\n")
    assert benchmark.cache_fingerprint(root, unit, task)[0] != fingerprint
    script.write_bytes(original)
    assert benchmark.cache_fingerprint(root, unit, task)[0] == fingerprint
    shutil.rmtree(result_path.parent)
    (root / "cache" / rel).unlink()


def assert_proficiency_cache_requires_exact_primary_trial_set() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit = {
            "evaluation": "llm_proficiency",
            "assigned_languages": ["Python"],
            "max_output_tokens_per_call": 16384,
        }
        record = {
            "certification": {
                "scored_output_cap": 16384,
                "trial_calls": 18,
                "cap_truncated_trial_calls": 0,
                "max_trial_output_tokens": 512,
                "proficiency_integrity": True,
            }
        }
        problem = benchmark.cache_cap_reuse_problem(root, record, unit)
        assert problem is not None and "Primary trial allocation" in problem, problem

        certification = record["certification"]
        certification["proficiency_primary_trial_set_sha256"] = (
            benchmark.proficiency_primary_trial_set_sha256(root)
        )
        certification["proficiency_primary_trial_count"] = len(
            benchmark.proficiency_required_trial_ids(root)
        )
        problem = benchmark.cache_cap_reuse_problem(root, record, unit)
        assert problem is not None and "prompt set" in problem, problem
        certification["proficiency_primary_prompt_set_sha256"] = (
            benchmark.proficiency_primary_prompt_set_sha256(root, "Python")
        )
        problem = benchmark.cache_cap_reuse_problem(root, record, unit)
        assert problem is not None and "toolchain" in problem, problem
        certification["proficiency_toolchain_evidence"] = True
        problem = benchmark.cache_cap_reuse_problem(root, record, unit)
        assert problem is not None and "per-completion" in problem, problem
        certification["proficiency_runtime_verification"] = True
        problem = benchmark.cache_cap_reuse_problem(root, record, unit)
        assert problem is not None and "hidden-oracle workload contract" in problem, problem
        certification["proficiency_workload_contract_sha256"] = (
            benchmark.proficiency_workload_contract_sha256(root)
        )
        assert benchmark.cache_cap_reuse_problem(root, record, unit) is None


def assert_execution_identity_paths_are_policy_authoritative() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        policy = benchmark.cache_policy(root)
        paths = benchmark.quidra_execution_input_paths(policy)
        baseline = policy["quidra_execution_identity"]["legacy_baseline"]
        assert set(paths) == set(baseline["git_objects"]), (paths, baseline)

        policy_path = root / "template/config/cache_policy.json"
        bad = json.loads(json.dumps(policy))
        bad["quidra_execution_identity"]["input_paths"].append(paths[0])
        benchmark.json_dump(policy_path, bad)
        try:
            benchmark.cache_policy(root)
        except benchmark.BenchmarkError as exc:
            assert "duplicates" in str(exc), exc
        else:
            raise AssertionError(
                "duplicate Quidra execution input path did not invalidate cache policy"
            )


def assert_quidra_execution_identity_reuse_guard() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        current = benchmark.current_quidra_execution_identity(root)
        assert current is not None
        record = {
            "fingerprint_payload": {"assigned_languages": ["Quidra"]},
            "compatibility": {"quidra_execution_identity": current},
            "provenance": {"run_id": "2026-09-23-fce5cfa-gh16"},
        }
        assert benchmark.cache_quidra_execution_reuse_problem(root, record) is None

        run = benchmark.json_load(root / "run.json")
        changed = json.loads(json.dumps(current))
        changed["git_objects"]["src"] = "f" * 40
        changed["sha256"] = "e" * 64
        run["evaluated"]["quidra_execution_identity"] = changed
        benchmark.json_dump(root / "run.json", run)
        problem = benchmark.cache_quidra_execution_reuse_problem(root, record)
        assert problem is not None and "implementation changed" in problem, problem

        run["evaluated"]["quidra_execution_identity"] = current
        benchmark.json_dump(root / "run.json", run)
        legacy = {
            "fingerprint_payload": {"assigned_languages": ["Quidra"]},
            "provenance": {"run_id": "2026-09-23-fce5cfa-gh16"},
        }
        assert benchmark.cache_quidra_execution_reuse_problem(root, legacy) is None
        legacy["provenance"]["run_id"] = "2026-09-22-56f2c65-gh3"
        problem = benchmark.cache_quidra_execution_reuse_problem(root, legacy)
        assert problem is not None and "not verified" in problem, problem


        # Same scored bytes do not make stale execution identity/certification
        # safe to keep after a fresh remeasurement.
        existing = {
            "result_sha256": "a" * 64,
            "compatibility": {"quidra_execution_identity": current},
            "certification": {"validator_pass": True, "epoch": 1},
        }
        refreshed_identity = json.loads(json.dumps(current))
        refreshed_identity["sha256"] = "b" * 64
        candidate = {
            "result_sha256": "a" * 64,
            "compatibility": {"quidra_execution_identity": refreshed_identity},
            "certification": {"validator_pass": True, "epoch": 2},
        }
        assert benchmark.cache_record_metadata_refresh_required(existing, candidate)
        assert not benchmark.cache_record_metadata_refresh_required(existing, existing)
        assert not benchmark.cache_record_metadata_refresh_required(
            existing, dict(candidate, result_sha256="c" * 64)
        )

def assert_recovery_cache_snapshot_refresh_is_pre_manifest_only() -> None:
    with tempfile.TemporaryDirectory() as td:
        source = Path(td) / "source"
        source.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=source, check=True)
        (source / ".gitignore").write_text("/.quidra-benchmark/\n", encoding="utf-8")

        root = source / ".quidra-benchmark"
        (root / "cache").mkdir(parents=True)
        (root / "work/root").mkdir(parents=True)
        evaluated = "a" * 40
        benchmark.json_dump(root / "run.json", {
            "schema_version": 1,
            "run_id": "recovery-contract",
            "workspace_root": "/quidra-benchmark",
            "evaluated": {"commit_sha": evaluated},
            "cache_tree_sha256": benchmark.sha256_tree(root / "cache"),
        })
        benchmark.write_host_workspace_sentinel(
            source, "recovery-contract", evaluated
        )

        (root / "cache" / "new-certified-record.json").write_text(
            '{"ok":true}\n', encoding="utf-8"
        )
        assert benchmark.cmd_refresh_cache_snapshot(argparse.Namespace(
            source_repo=str(source), expected_commit=evaluated
        )) == 0
        run = benchmark.json_load(root / "run.json")
        assert run["cache_tree_sha256"] == benchmark.sha256_tree(root / "cache")

        benchmark.json_dump(root / "work/root/manifest.json", {"schema_version": 1})
        try:
            benchmark.cmd_refresh_cache_snapshot(argparse.Namespace(
                source_repo=str(source), expected_commit=evaluated
            ))
        except benchmark.BenchmarkError as exc:
            assert "before manifest freeze" in str(exc), exc
        else:
            raise AssertionError("cache snapshot refresh was allowed after manifest freeze")


def assert_cached_validator_rejection_becomes_miss() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)
        fingerprint = install_cache_record(root, unit, task)
        rel = benchmark.cache_record_relative(unit, fingerprint)
        path = root / "cache" / rel
        record = benchmark.json_load(path)
        # Worker-provided arithmetic is deliberately non-authoritative:
        # current validation recomputes it from evidence, so a stale/bogus
        # requirements score alone must NOT force a paid rerun. Corrupt the
        # underlying rubric evidence instead; that cannot be repaired
        # mechanically and must become a leaf-local cache invalidation.
        record["result"]["requirements"] = {
            "metric.documentation_quality": {"Python": 999.0}
        }
        evidence = record["result"]["evidence"]["metric.documentation_quality"]
        first_component = next(iter(evidence["component_levels"]))
        evidence["component_levels"][first_component] = 9
        raw = json.dumps(
            record["result"], sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        record["result_sha256"] = benchmark.sha256_bytes(raw)
        benchmark.json_dump(path, record)

        assert benchmark.hydrate_certified_cache(root) == 0
        ledger = benchmark.json_load(root / "work/root/ledger.json")
        assert ledger["units"][unit["id"]]["status"] == "PENDING", ledger
        agent = root / "work/agents" / unit["assigned_agent_id"]
        assert not (agent / "result.json").exists()
        assert not (agent / "cache_receipt.json").exists()
        status = benchmark.json_load(root / "results/cache_status.json")
        assert "rejected by current validator" in status["misses"][unit["id"]]["reason"]

        install_cache_record(root, unit, task)
        assert benchmark.hydrate_certified_cache(root) == 1
        status = benchmark.json_load(root / "results/cache_status.json")
        assert unit["id"] in status["hits"] and unit["id"] not in status["misses"]


def assert_language_quality_design_runner_owned_scoring() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        asset = benchmark.language_quality_design_rubric_asset(root)
        assert asset["frozen"] is True
        assert len(asset["metrics"]) == 8
        reqs = ["metric.readability", "metric.diagnostics"]
        task = {
            "evaluation": "language_quality",
            "assigned_languages": ["Python"],
            "requirement_ids": reqs,
        }
        evidence = {}
        for index, rid in enumerate(reqs):
            rubric = asset["metrics"][rid]
            component_ids = [row["id"] for row in rubric["components"]]
            level = 4 if index == 0 else 3
            evidence[rid] = {
                "rubric_id": rubric["rubric_id"],
                "component_levels": {cid: level for cid in component_ids},
                "component_findings": {
                    cid: f"frozen test evidence for {cid}" for cid in component_ids
                },
                "evidence_refs": [
                    "template/methodology-assets/language_quality/design_rubrics.json"
                ],
                "selection_rule": rubric["selection_rule"],
                "limitations": "",
            }
        result = {
            "schema_version": 1,
            "evaluation": "language_quality",
            "requirements": {rid: {"Python": -1.0} for rid in reqs},
            "evidence": evidence,
        }
        benchmark.apply_language_quality_design_runner_scores(root, task, result)
        assert result["requirements"][reqs[0]] == {"Python": 100.0}
        assert result["requirements"][reqs[1]] == {"Python": 75.0}
        scoring = result["evidence"]["language_quality_design_runner_scoring"]
        assert scoring["rubric_set_id"] == "language-quality-design-runner-rubric-v1"

        broken = json.loads(json.dumps(result))
        broken["evidence"][reqs[0]]["component_levels"].pop(
            next(iter(broken["evidence"][reqs[0]]["component_levels"]))
        )
        try:
            benchmark.apply_language_quality_design_runner_scores(
                root, task, broken
            )
        except benchmark.BenchmarkError as exc:
            assert "component_levels" in str(exc), exc
        else:
            raise AssertionError(
                "incomplete Language Quality design component evidence was accepted"
            )

        external = json.loads(json.dumps(result))
        external["evidence"][reqs[0]]["evidence_refs"] = [
            "https://example.invalid/not-a-frozen-input"
        ]
        try:
            benchmark.apply_language_quality_design_runner_scores(
                root, task, external
            )
        except benchmark.BenchmarkError as exc:
            assert "relative repo/... or template/... path" in str(exc), exc
        else:
            raise AssertionError(
                "external Language Quality evidence reference was accepted"
            )


def assert_ecosystem_runner_owned_scoring() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        asset = benchmark.ecosystem_rubric_asset(root)
        frozen_snapshot_date = str(
            asset["evidence_policy"]["snapshot_date"]
        )
        assert asset["frozen"] is True
        assert len(asset["metrics"]) == 15
        reqs = [
            "metric.installation_distribution_experience",
            "metric.toolchain_stability_release_maturity",
        ]
        task = {
            "evaluation": "ecosystem",
            "assigned_languages": ["Python"],
            "requirement_ids": reqs,
        }
        evidence = {}
        for index, rid in enumerate(reqs):
            rubric = asset["metrics"][rid]
            component_ids = [row["id"] for row in rubric["components"]]
            level = 4 if index == 0 else 3
            evidence[rid] = {
                "rubric_id": rubric["rubric_id"],
                "component_levels": {cid: level for cid in component_ids},
                "component_findings": {
                    cid: f"verified evidence for {cid}" for cid in component_ids
                },
                "sources": ["https://example.invalid/evidence"],
                "candidate_universe": "frozen test candidate universe",
                "selection_rule": rubric["selection_rule"],
                "retrieval_route": "provider-brokered web search",
                "snapshot_date": frozen_snapshot_date,
                "limitations": "",
            }
        result = {
            "schema_version": 1,
            "evaluation": "ecosystem",
            "requirements": {
                rid: {"Python": -1} for rid in reqs
            },
            "evidence": evidence,
        }
        benchmark.apply_ecosystem_runner_scores(root, task, result)
        assert result["requirements"][reqs[0]] == {"Python": 100.0}
        assert result["requirements"][reqs[1]] == {"Python": 75.0}
        assert (
            result["evidence"]["runner_scoring"]["rubric_set_id"]
            == "ecosystem-runner-rubric-v2"
        )

        broken = json.loads(json.dumps(result))
        broken["evidence"][reqs[0]]["component_levels"].pop(
            next(iter(broken["evidence"][reqs[0]]["component_levels"]))
        )
        try:
            benchmark.apply_ecosystem_runner_scores(root, task, broken)
        except benchmark.BenchmarkError as exc:
            assert "component_levels" in str(exc), exc
        else:
            raise AssertionError("incomplete Ecosystem component evidence was accepted")

        wrong_date = json.loads(json.dumps(result))
        wrong_day = (
            frozen_snapshot_date[:-2] + "01"
            if not frozen_snapshot_date.endswith("01")
            else frozen_snapshot_date[:-2] + "02"
        )
        wrong_date["evidence"][reqs[0]]["snapshot_date"] = wrong_day
        try:
            benchmark.apply_ecosystem_runner_scores(root, task, wrong_date)
        except benchmark.BenchmarkError as exc:
            assert "must equal the frozen Ecosystem snapshot date" in str(exc), exc
        else:
            raise AssertionError("non-frozen Ecosystem snapshot was accepted")

        policy_path = root / "template/config/cache_policy.json"
        policy_bytes = policy_path.read_bytes()
        policy = benchmark.json_load(policy_path)
        policy["declared_epochs"]["ecosystem"] = "2026-10"
        benchmark.json_dump(policy_path, policy)
        try:
            benchmark.apply_ecosystem_runner_scores(root, task, result)
        except benchmark.BenchmarkError as exc:
            assert "advance both together" in str(exc), exc
        else:
            raise AssertionError("Ecosystem epoch/snapshot drift was accepted")
        policy_path.write_bytes(policy_bytes)

        gateway_path = root / "template/config/inference_gateway.json"
        gateway_bytes = gateway_path.read_bytes()
        gateway = benchmark.json_load(gateway_path)
        gateway["anthropic_web_search"]["max_uses_per_request"] = 4
        benchmark.json_dump(gateway_path, gateway)
        try:
            benchmark.ecosystem_rubric_asset(root)
        except benchmark.BenchmarkError as exc:
            assert "max_uses_per_request" in str(exc), exc
        else:
            raise AssertionError("Ecosystem/gateway search-budget drift was accepted")
        gateway_path.write_bytes(gateway_bytes)



def assert_budget_plan_excludes_complete_units() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)
        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        assert plan["pending_agent_units"] == 1, plan
        assert plan["estimated_uncached_usd"] > 0, plan
        assert plan["sufficient"] is True, plan

        ledger_path = root / "work/root/ledger.json"
        ledger = benchmark.json_load(ledger_path)
        ledger["units"][unit["id"]]["status"] = "COMPLETE"
        ledger["units"][unit["id"]]["validation_result"] = "PASS"
        benchmark.json_dump(ledger_path, ledger)
        cached = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=0.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
            smoke_reserve_usd=0.10,
        )
        assert cached["pending_agent_units"] == 0, cached
        assert cached["estimated_uncached_usd"] == 0, cached
        assert cached["expected_paid_api_calls_upper_bound"] == 0, cached
        assert cached["provider_smoke_required"] is False, cached
        assert cached["smoke_reserve_usd"] == 0, cached
        assert cached["recommended_budget_usd"] == 0, cached
        assert cached["sufficient"] is True, cached



def assert_budget_plan_exposes_configured_retry_ceiling() -> None:
    """The preflight must distinguish nominal spend from every allowed retry."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, _ = create_cacheable_task(root)
        freeze_manifest(root, unit)
        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        row = plan["units"][0]
        max_turns = int(
            benchmark.json_load(root / "template/config/sandbox_agent.json")[
                "max_turns"
            ]
        )
        assert row["planned_calls_estimate"] == 1 + min(3, max_turns), row
        assert row["remaining_attempts"] == 3, row
        assert row["calls_upper_bound"] == 3 * (1 + max_turns), row
        assert (
            plan["planned_paid_api_calls_estimate"]
            < plan["expected_paid_api_calls_upper_bound"]
        ), plan
        assert (
            plan["estimated_uncached_usd"]
            < plan["retry_ceiling_uncached_usd"]
        ), plan
        assert (
            plan["recommended_budget_usd"]
            < plan["full_retry_envelope_recommended_budget_usd"]
        ), plan

    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, _ = create_cacheable_task(root)
        unit["worker_mode"] = "packet-only"
        unit["max_llm_calls"] = 0
        freeze_manifest(root, unit)
        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        row = plan["units"][0]
        assert row["planned_calls_estimate"] == 1, row
        assert row["calls_upper_bound"] == 3, row

        ledger_path = root / "work/root/ledger.json"
        ledger = benchmark.json_load(ledger_path)
        ledger["units"][unit["id"]]["attempts"] = 1
        benchmark.json_dump(ledger_path, ledger)
        resumed = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        resumed_row = resumed["units"][0]
        assert resumed_row["remaining_attempts"] == 2, resumed_row
        assert resumed_row["calls_upper_bound"] == 2, resumed_row


def assert_empty_cache_impact_is_a_valid_first_run() -> None:
    """A repository with no v1 records still produces a zero-impact plan."""
    with tempfile.TemporaryDirectory() as td:
        source = Path(td) / "source"
        source.mkdir()
        summary = benchmark.cache_impact(source)
        assert summary["valid"] == 0, summary
        assert summary["invalid"] == [], summary


def assert_evaluation_scoped_primary_cache() -> None:
    """Unrelated Primary settings neither re-key nor reprompt another evaluation.

    Historical full-primary packets are accepted only through the narrow
    projection migration: every non-scoped prompt component and every other
    cache dependency must still match.
    """
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        primary_path = root / "template/config/primary.json"
        original_primary = primary_path.read_bytes()

        ecosystem_before = benchmark.primary_config_projection_sha256(
            root, "ecosystem"
        )
        proficiency_before = benchmark.primary_config_projection_sha256(
            root, "llm_proficiency"
        )
        language_quality_before = benchmark.primary_config_projection_sha256(
            root, "language_quality"
        )
        primary = benchmark.json_load(primary_path)
        # This pre-certified-cache flag is retained in the source config only
        # for historical readability. It is not a scientific dependency and
        # correcting it must not throw away paid Language Quality evidence.
        primary["language_quality"]["reuse_never_includes_measurements"] = (
            not bool(
                primary["language_quality"].get(
                    "reuse_never_includes_measurements", False
                )
            )
        )
        benchmark.json_dump(primary_path, primary)
        assert benchmark.primary_config_projection_sha256(
            root, "language_quality"
        ) == language_quality_before, (
            "legacy measurement-reuse prose re-keyed Language Quality"
        )
        primary_path.write_bytes(original_primary)

        primary = benchmark.json_load(primary_path)
        primary["llm_proficiency"]["primary_prompt_variants"].append(
            "cache-scope-regression"
        )
        benchmark.json_dump(primary_path, primary)
        assert benchmark.primary_config_projection_sha256(
            root, "ecosystem"
        ) == ecosystem_before, (
            "a Proficiency-only setting re-keyed Ecosystem"
        )
        assert benchmark.primary_config_projection_sha256(
            root, "llm_proficiency"
        ) != proficiency_before, (
            "a Proficiency setting failed to re-key Proficiency"
        )
        primary_path.write_bytes(original_primary)

        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)
        current = benchmark.cache_fingerprint(root, unit, task)
        assert current is not None
        current_fingerprint, current_payload = current

        # Seed a synthetic pre-migration record whose packet embedded the whole
        # primary.json.  Its semantic Ecosystem projection is identical.
        full_primary_text = primary_path.read_text(encoding="utf-8")
        full_primary_sha = benchmark.sha256_file(primary_path)
        legacy_section = (
            "\n\n---\n\n"
            "## Embedded input: primary.json\n"
            f"Source SHA-256: `{full_primary_sha}`\n\n"
            + full_primary_text
        )
        legacy_component = benchmark.store_prompt_component(
            root, legacy_section, "embedded:primary.json"
        )
        legacy_components = [
            legacy_component
            if component.get("kind") == "embedded:primary.json"
            else component
            for component in task["prompt_components"]
        ]

        # A historical prompt manifest can prove a safe scoped-cache migration
        # only when every component it names is present in the canonical prompt
        # store. Mirror the real finalized-prompt layout instead of seeding only
        # primary.json and turning missing fixture bytes into a false cache MISS.
        canonical_components = root / "template/prompts/components/by-hash"
        canonical_components.mkdir(parents=True, exist_ok=True)
        for component in legacy_components:
            source = Path(component["path"])
            destination = canonical_components / f"{component['sha256']}.md"
            if not destination.is_file():
                destination.write_bytes(source.read_bytes())

        legacy_rendered = b"".join(
            Path(component["path"]).read_bytes()
            for component in legacy_components
        )
        legacy_prompt_sha = benchmark.sha256_bytes(legacy_rendered)
        legacy_manifest = {
            "schema_version": 1,
            "prompt_sha256": legacy_prompt_sha,
            "rendered_bytes": len(legacy_rendered),
            "components": [
                {
                    "kind": component["kind"],
                    "sha256": component["sha256"],
                    "bytes": component["bytes"],
                }
                for component in legacy_components
            ],
        }
        benchmark.json_dump(
            root
            / "template/prompts/manifests/by-hash"
            / f"{legacy_prompt_sha}.json",
            legacy_manifest,
        )

        installed = install_cache_record(root, unit, task)
        assert installed == current_fingerprint
        current_record_path = (
            root / "cache" / benchmark.cache_record_relative(unit, current_fingerprint)
        )
        record = benchmark.json_load(current_record_path)
        current_record_path.unlink()

        legacy_payload = dict(current_payload)
        legacy_hashes = dict(legacy_payload["unit_input_hashes"])
        legacy_hashes["primary_config"] = full_primary_sha
        legacy_payload["unit_input_hashes"] = legacy_hashes
        legacy_payload["exact_task_packet_sha256"] = legacy_prompt_sha
        legacy_fingerprint = benchmark.sha256_bytes(
            json.dumps(
                legacy_payload,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=False,
            ).encode("utf-8")
        )
        record["fingerprint"] = legacy_fingerprint
        record["fingerprint_payload"] = legacy_payload
        record["provenance"]["prompt_sha256"] = legacy_prompt_sha
        legacy_record_path = (
            root / "cache" / benchmark.cache_record_relative(unit, legacy_fingerprint)
        )
        benchmark.json_dump(legacy_record_path, record)
        run = benchmark.json_load(root / "run.json")
        run["cache_tree_sha256"] = benchmark.sha256_tree(root / "cache")
        benchmark.json_dump(root / "run.json", run)

        hits = benchmark.hydrate_certified_cache(root)
        assert hits == 1, "the compatible already-paid record was not reused"
        receipt = benchmark.json_load(
            root / "work/agents" / unit["assigned_agent_id"] / "cache_receipt.json"
        )
        assert receipt["fingerprint"] == current_fingerprint
        assert receipt["source_fingerprint"] == legacy_fingerprint
        assert receipt["compatibility_mode"] == "scoped-input-projection"



def assert_corrupt_cache_is_leaf_local_and_explicit() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)
        fingerprint = install_cache_record(root, unit, task)
        record_path = root / "cache" / benchmark.cache_record_relative(
            unit, fingerprint
        )
        record_path.write_text("{ definitely-not-json", encoding="utf-8")

        # A single damaged paid record must invalidate only its leaf, never
        # abort cache hydration for the whole benchmark.
        hits = benchmark.hydrate_certified_cache(root)
        assert hits == 0
        ledger = benchmark.json_load(root / "work/root/ledger.json")
        assert ledger["units"][unit["id"]]["status"] == "PENDING"
        status = benchmark.json_load(root / "results/cache_status.json")
        assert unit["id"] in status["misses"], status
        assert unit["id"] in status["invalidated"], status
        reason = status["invalidated"][unit["id"]]["reason"]
        assert "corrupt" in reason or "unreadable" in reason, reason


def assert_execution_plan_classifies_cache_decisions() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)

        # Materialize the exact MISS before pricing, as production prepare does.
        assert benchmark.hydrate_certified_cache(root) == 0
        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        new_ids = {
            row["work_unit_id"]
            for row in plan["execution_decisions"]["new_paid_execution"]
        }
        assert unit["id"] in new_ids, plan["execution_decisions"]
        assert plan["expected_paid_api_calls_upper_bound"] > 0, plan
        assert plan["cache_invalidated_units"] == 0, plan

        # A real invalidation is classified separately from a first execution.
        status = benchmark.json_load(root / "results/cache_status.json")
        status["invalidated"] = {
            unit["id"]: {
                "fingerprint": "x" * 64,
                "scope": "python",
                "reason": "validator contract changed",
            }
        }
        status["misses"][unit["id"]] = dict(status["invalidated"][unit["id"]])
        benchmark.json_dump(root / "results/cache_status.json", status)
        invalidated_plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        reevaluate_ids = {
            row["work_unit_id"]
            for row in invalidated_plan["execution_decisions"][
                "paid_reevaluation_after_invalidation"
            ]
        }
        assert unit["id"] in reevaluate_ids, invalidated_plan["execution_decisions"]


def assert_packet_paid_response_commit_is_replayable() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, _ = create_cacheable_task(root)
        agent_id = unit["assigned_agent_id"]
        task_path = root / "work/agents" / agent_id / "task.json"
        task = benchmark.json_load(task_path)
        task["worker_mode"] = "packet-only"
        benchmark.json_dump(task_path, task)

        worker = {
            "schema_version": 1,
            "task_id": agent_id,
            "files": [{
                "path": "result.json",
                "json": {
                    "schema_version": 1,
                    "evaluation": "ecosystem",
                    "requirements": {
                        "metric.documentation_quality": {"Python": 75.0}
                    },
                    "evidence": {"test": "paid reply persisted before validation"},
                },
            }],
        }
        provider_response = {
            "content": json.dumps(worker),
            "stop_reason": "end_turn",
            "usage": {"input_tokens": 10, "output_tokens": 20},
            "decoding": {"temperature": 0},
        }
        paid = benchmark.persist_packet_paid_response(
            task_path.parent, task, provider_response
        )
        assert paid.is_file(), paid
        preserved = benchmark.json_load(paid)
        assert preserved["response"] == provider_response

        raw = json.dumps(worker, sort_keys=True).encode("utf-8")
        first = benchmark.apply_worker_response(root, agent_id, raw)
        second = benchmark.apply_worker_response(root, agent_id, raw)
        assert first["response_sha256"] == second["response_sha256"]
        assert (task_path.parent / "result.json").is_file()



def assert_partial_paid_checkpoint_roundtrip() -> None:
    with tempfile.TemporaryDirectory() as td:
        base = Path(td)
        root = make_workspace(base)
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)
        agent_id = unit["assigned_agent_id"]
        agent_dir = root / "work/agents" / agent_id
        trial_dir = agent_dir / "trials/case-t1"
        trial_dir.mkdir(parents=True)
        prompt = "write a tiny program"
        completion = "print('paid once')"
        (trial_dir / "prompt_01.txt").write_text(prompt, encoding="utf-8")
        (trial_dir / "completion_01.txt").write_text(completion, encoding="utf-8")
        benchmark.json_dump(
            trial_dir / "session.json",
            {
                "schema_version": 1,
                "trial_id": "case-t1",
                "calls": [{
                    "prompt": prompt,
                    "prompt_sha256": benchmark.sha256_bytes(prompt.encode()),
                    "completion_sha256": benchmark.sha256_bytes(completion.encode()),
                    "prompt_path": "trials/case-t1/prompt_01.txt",
                    "completion_path": "trials/case-t1/completion_01.txt",
                    "verification": None,
                    "verification_path": None,
                }],
            },
        )
        benchmark.json_dump(
            agent_dir / "trial_call_journal.json",
            {
                "schema_version": 1,
                "calls": [{
                    "trial_id": "case-t1",
                    "call": 1,
                    "action": "trial_start",
                    "prompt_sha256": benchmark.sha256_bytes(prompt.encode()),
                    "completion_sha256": benchmark.sha256_bytes(completion.encode()),
                    "verification": None,
                    "verification_path": None,
                }],
            },
        )
        benchmark.json_dump(
            agent_dir / "agent_trace.partial.json",
            {
                "schema_version": 1,
                "agent_id": agent_id,
                "worker_mode": "sandbox-agent",
                "prompt_sha256": task["prompt_sha256"],
                "trace": [{
                    "turn": 1,
                    "action": "trial_start",
                    "observation": {"ok": True, "trial_id": "case-t1", "call": 1},
                }],
            },
        )

        store = base / "paid-state"
        exported = benchmark.export_partial_paid_checkpoints(root, store)
        assert exported["updated_unit_count"] == 1, exported
        assert exported["exported_units"][0]["paid_call_count"] == 1

        shutil.rmtree(agent_dir / "trials")
        (agent_dir / "trial_call_journal.json").unlink()
        (agent_dir / "agent_trace.partial.json").unlink()

        restored = benchmark.import_partial_paid_checkpoints(root, store)
        assert restored["imported_unit_count"] == 1, restored
        assert restored["restored_paid_calls"] == 1, restored
        assert (trial_dir / "completion_01.txt").read_text() == completion
        assert (agent_dir / "trial_call_journal.json").is_file()
        assert (agent_dir / "resume_trace.json").is_file()

        # Re-exporting identical state must not generate a new cache save.
        again = benchmark.export_partial_paid_checkpoints(root, store)
        assert again["updated_unit_count"] == 0, again

        # Raw paid calls remain durable after the leaf itself becomes COMPLETE.
        # The certified result cache owns the validated result; this separate
        # exact-fingerprint store lets a later validator/grader revision replay
        # the paid calls without buying the same model inference again.
        ledger_path = root / "work/root/ledger.json"
        ledger = benchmark.json_load(ledger_path)
        ledger["units"][unit["id"]]["status"] = "COMPLETE"
        ledger["units"][unit["id"]]["validation_result"] = "PASS"
        benchmark.json_dump(ledger_path, ledger)
        complete_store = base / "paid-state-complete"
        complete_export = benchmark.export_partial_paid_checkpoints(
            root, complete_store
        )
        assert complete_export["updated_unit_count"] == 1, complete_export
        assert complete_export["exported_units"][0]["paid_call_count"] == 1

        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=100.0,
            evaluation="ecosystem",
            safety_multiplier=1.25,
        )
        # The unit declares one scored call. Restoring that paid call leaves
        # only the bounded orchestration envelope, not another scored trial.
        assert plan["partial_paid_restored_calls"] == 1, plan
        assert unit["id"] in plan["partial_paid_resumed_units"], plan


def assert_optional_units_do_not_enter_required_resume_set() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, _ = create_cacheable_task(root)
        unit["required_for_complete"] = False
        freeze_manifest(root, unit)
        benchmark.json_dump(
            root / "results/primary_status.json",
            {"schema_version": 1, "evaluations": {}},
        )
        audit = benchmark.build_completeness_audit(root)
        assert audit["required_incomplete_units"] == [], audit
        assert [row["work_unit_id"] for row in audit["optional_incomplete_units"]] == [
            unit["id"]
        ]



def assert_certified_checkpoint_promotes_prompt_dependencies() -> None:
    """A partial run must preserve prompt bytes needed to reuse paid cache later."""
    with tempfile.TemporaryDirectory() as td:
        base = Path(td)
        root = make_workspace(base)
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)

        promotion = {"records": [{"work_unit_id": unit["id"]}]}
        prompt_hashes = benchmark.cache_record_prompt_hashes(root, promotion)
        assert prompt_hashes == {task["prompt_sha256"]}, prompt_hashes

        # A non-selected malformed manifest proves checkpoint promotion is scoped
        # to certified records rather than copying every generated prompt.
        benchmark.json_dump(
            root / "prompts/manifests/ignored.json",
            {
                "schema_version": 1,
                "prompt_sha256": "f" * 64,
                "components": [{"kind": "ignored", "sha256": "not-a-digest"}],
            },
        )
        source = base / "source"
        (source / "benchmark/template/prompts").mkdir(parents=True)
        summary = benchmark.promote_prompt_store(
            source, root, prompt_hashes=prompt_hashes
        )
        assert summary["manifests"] == 1, summary

        stored = (
            source / "benchmark/template/prompts/manifests/by-hash"
            / f"{task['prompt_sha256']}.json"
        )
        assert stored.is_file(), stored
        manifest = benchmark.json_load(stored)
        for component in manifest["components"]:
            digest = component["sha256"]
            assert (
                source / "benchmark/template/prompts/components/by-hash"
                / f"{digest}.md"
            ).is_file(), digest
        assert not (
            source / "benchmark/template/prompts/manifests/by-hash"
            / f"{'f' * 64}.json"
        ).exists()


def main() -> None:
    assert_certified_checkpoint_promotes_prompt_dependencies()
    assert_partial_paid_checkpoint_roundtrip()
    assert_optional_units_do_not_enter_required_resume_set()
    assert_corrupt_cache_is_leaf_local_and_explicit()
    assert_execution_plan_classifies_cache_decisions()
    assert_packet_paid_response_commit_is_replayable()
    assert_empty_cache_impact_is_a_valid_first_run()
    assert_evaluation_scoped_primary_cache()
    assert_budget_plan_excludes_complete_units()
    assert_budget_plan_exposes_configured_retry_ceiling()
    assert_accepted_trial_start_marks_the_scored_boundary()
    assert_language_quality_design_runner_owned_scoring()
    assert_ecosystem_runner_owned_scoring()
    assert_execution_identity_paths_are_policy_authoritative()
    assert_quidra_execution_identity_reuse_guard()
    assert_cached_validator_rejection_becomes_miss()
    assert_proficiency_cache_requires_exact_primary_trial_set()
    with tempfile.TemporaryDirectory() as mechanical_td:
        # Its own workspace: the test freezes a manifest of one mechanical unit.
        assert_mechanical_measurements_are_cacheable(make_workspace(Path(mechanical_td)), Path(mechanical_td))
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        assert_language_scoped_program_reads(root)
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)
        assert_scored_cap_governs_reuse(root, unit, task)
        assert_readiness_audits_are_cacheable(root, task)

        # Production tasks are authored inside /quidra-benchmark, while post-run
        # promotes them from the host staging path. Both path spellings must hash
        # the same readable content and therefore produce the same cache key.
        host_task = dict(task)
        canonical_task = dict(task)
        canonical_task["read_paths"] = [
            str(benchmark.CANONICAL_WORKSPACE / "template/workloads/micro.md")
        ]
        assert (
            benchmark.cache_read_input_hashes(root, host_task)
            == benchmark.cache_read_input_hashes(root, canonical_task)
        )
        benchmark.json_dump(
            root / "work/agents" / unit["assigned_agent_id"] / "task.json",
            canonical_task,
        )
        task = canonical_task

        original = benchmark.cache_fingerprint(root, unit, task)
        assert original is not None
        original_fingerprint = original[0]
        assert benchmark.cache_eligible_unit(root, unit)

        # Quidra is cacheable, keyed by the versions its snapshot declares.
        quidra = {**unit, "assigned_languages": ["Quidra"]}
        assert benchmark.cache_eligible_unit(root, quidra)
        (root / "repo" / "project.toml").write_text(
            'name = "Quidra"\nversion = "0.2.1"\nlanguage_version = "0.1"\n', encoding="utf-8"
        )
        quidra_pair = benchmark.cache_fingerprint(root, quidra, task)
        assert quidra_pair is not None, "a Quidra unit produced no cache key"
        assert quidra_pair[1]["quidra_target"] == {"version": "0.2.1", "language_version": "0.1"}
        assert "quidra_target" not in original[1], "a comparison unit carried a Quidra identity"
        (root / "repo" / "project.toml").write_text(
            'name = "Quidra"\nversion = "0.2.2"\nlanguage_version = "0.1"\n', encoding="utf-8"
        )
        assert benchmark.cache_fingerprint(root, quidra, task)[0] != quidra_pair[0], (
            "bumping the Quidra version must change the key"
        )
        (root / "repo" / "project.toml").write_text(
            'name = "Quidra"\nversion = "0.2.1"\nlanguage_version = "0.1"\n', encoding="utf-8"
        )
        assert benchmark.cache_fingerprint(root, quidra, task)[0] == quidra_pair[0]

        # Every material execution input that matters must invalidate reuse.
        run = benchmark.json_load(root / "run.json")
        run["inference_identity"]["model"] = "different-model"
        benchmark.json_dump(root / "run.json", run)
        assert benchmark.cache_fingerprint(root, unit, task)[0] != original_fingerprint
        run["inference_identity"]["model"] = "claude-sonnet-5"
        benchmark.json_dump(root / "run.json", run)

        toolchains = benchmark.json_load(root / "results/toolchains.json")
        toolchains["toolchains"]["Python"]["canonical"] = "3.15.0"
        benchmark.json_dump(root / "results/toolchains.json", toolchains)
        assert benchmark.cache_fingerprint(root, unit, task)[0] != original_fingerprint
        toolchains["toolchains"]["Python"]["canonical"] = "3.14.5"
        benchmark.json_dump(root / "results/toolchains.json", toolchains)

        changed_task = dict(task)
        changed_task["prompt_sha256"] = "f" * 64
        assert (
            benchmark.cache_fingerprint(root, unit, changed_task)[0]
            != original_fingerprint
        )

        readable = root / "template/workloads/micro.md"
        original_bytes = readable.read_bytes()
        packet_before = task["prompt_sha256"]
        readable.write_bytes(original_bytes + b"\ncache-fingerprint-mutation\n")
        assert task["prompt_sha256"] == packet_before
        assert benchmark.cache_fingerprint(root, unit, task)[0] != original_fingerprint
        readable.write_bytes(original_bytes)
        assert benchmark.cache_fingerprint(root, unit, task)[0] == original_fingerprint

        # The ecosystem epoch is declared by a person, not by the calendar: the
        # key carries the declared value, so records survive a month boundary
        # and miss only when the operator changes the value.
        epoch_policy = benchmark.cache_policy(root)
        for evaluation in ("ecosystem", "semantic_compression", "llm_proficiency"):
            assert benchmark.cache_epoch(root, evaluation) == (
                epoch_policy["declared_epochs"][evaluation]
            )
        assert benchmark.cache_epoch(root, "llm_learnability") == "stable"
        policy_path = root / "template/config/cache_policy.json"
        policy_bytes = policy_path.read_bytes()
        policy = benchmark.json_load(policy_path)
        policy["declared_epochs"]["ecosystem"] = "2026-12"
        benchmark.json_dump(policy_path, policy)
        assert benchmark.cache_fingerprint(root, unit, task)[0] != original_fingerprint, (
            "changing the declared ecosystem epoch must change the key"
        )
        # Restore the exact bytes: the template tree hash is part of run integrity.
        policy_path.write_bytes(policy_bytes)
        assert benchmark.cache_fingerprint(root, unit, task)[0] == original_fingerprint

        installed = install_cache_record(root, unit, task)
        assert installed == original_fingerprint

        hits = benchmark.hydrate_certified_cache(root)
        assert hits == 1
        ledger = benchmark.json_load(root / "work/root/ledger.json")
        assert ledger["units"][unit["id"]]["status"] == "COMPLETE"
        result = benchmark.json_load(
            root / "work/agents" / unit["assigned_agent_id"] / "result.json"
        )
        assert result["requirements"]["metric.documentation_quality"] == {"Python": 75.0}
        receipt = benchmark.json_load(
            root / "work/agents" / unit["assigned_agent_id"] / "cache_receipt.json"
        )
        assert receipt["status"] == "HIT"
        assert receipt["fingerprint"] == original_fingerprint
        status = benchmark.json_load(root / "results/cache_status.json")
        assert status["hit_count"] == 1

        # COMPLETE cache hits must not even receive permission to make a paid call.
        policy = production.write_policy(
            root, root / "results/task-policy.json", model="claude-sonnet-5"
        )
        assert policy["tasks"] == {}
        assert policy["cache"]["hit_count"] == 1
        assert policy["cache"]["paid_task_count"] == 0
        assert policy["cache"]["units_skipped_as_complete"] == [unit["id"]]

        # A COMPLETE+PASS non-Quidra unit is independently promotable even when
        # its Primary evaluation is still PARTIAL. This is what makes a paid run
        # resumable after a budget/provider/wall-clock stop.
        source = Path(td) / "source"
        (source / "benchmark/cache").mkdir(parents=True)
        (source / "benchmark/template/prompts/components/by-hash").mkdir(parents=True)
        (source / "benchmark/template/prompts/manifests/by-hash").mkdir(parents=True)
        benchmark.json_dump(
            root / "results/primary_status.json",
            {
                "schema_version": 1,
                "evaluations": {
                    "ecosystem": {
                        "status": "PARTIAL",
                        "scoreable": True,
                        "scores": {"Python": 75.0},
                        "ranking": [{"language": "Python", "score": 75.0}],
                        "blockers": [],
                    }
                },
            },
        )
        cache_promotion = benchmark.promote_certified_cache(source, root)
        assert cache_promotion["promoted"] == 1, cache_promotion
        promoted_record = source / "benchmark/cache" / cache_promotion["records"][0]["path"]
        assert promoted_record.is_file()
        promoted = benchmark.json_load(promoted_record)
        assert promoted["certification"]["unit_complete"] is True
        assert promoted["certification"]["primary_complete"] is False
        policy_cfg = benchmark.cache_policy(root)["promotion"]
        assert policy_cfg["require_complete_primary_evaluation"] is False
        assert policy_cfg["checkpoint_completed_units"] is True

        prompt_promotion = benchmark.promote_prompt_store(source, root)
        assert prompt_promotion["components"] > 0, prompt_promotion
        assert prompt_promotion["manifests"] == 1, prompt_promotion
        canonical_manifest = (
            source / "benchmark/template/prompts/manifests/by-hash"
            / f"{task['prompt_sha256']}.json"
        )
        canonical = benchmark.json_load(canonical_manifest)
        assert canonical["prompt_sha256"] == task["prompt_sha256"]
        assert "agent_id" not in canonical
        assert "evaluation" not in canonical

        # Promotion is content-addressed and idempotent.
        assert benchmark.promote_certified_cache(source, root)["promoted"] == 0
        second_prompts = benchmark.promote_prompt_store(source, root)
        assert second_prompts == {"components": 0, "manifests": 0}, second_prompts

        # A unit the run measured again although a record already sat at its
        # key replaces that record. The scored cap is deliberately outside the
        # key, so a trial cut off under an older cap re-measures to the same
        # fingerprint with a different result; the first full benchmark run
        # lost its entire checkpoint, and with it every record of a 40 USD
        # measurement, because that case raised instead.
        agent_dir = root / "work/agents" / unit["assigned_agent_id"]
        receipt_path = agent_dir / "cache_receipt.json"
        receipt_content = benchmark.json_load(receipt_path)
        measured = benchmark.json_load(agent_dir / "result.json")
        measured["evidence"] = {"certified": "measured again under the current cap"}
        benchmark.json_dump(agent_dir / "result.json", measured)
        receipt_path.unlink()
        remeasured = benchmark.promote_certified_cache(source, root)
        assert remeasured["replaced"] == 1, remeasured
        assert remeasured["promoted"] == 0, remeasured
        assert remeasured["skipped"] == [], remeasured
        stored = benchmark.json_load(promoted_record)
        assert stored["result"]["evidence"]["certified"].endswith("current cap")

        # A unit the run hydrated cannot legitimately differ from the record it
        # came from. Report that one and keep the stored record, but still
        # checkpoint everything else the run paid for.
        benchmark.json_dump(receipt_path, receipt_content)
        measured["evidence"] = {"certified": "disagrees with its own cache hit"}
        benchmark.json_dump(agent_dir / "result.json", measured)
        mismatched = benchmark.promote_certified_cache(source, root)
        assert mismatched["promoted"] == 0 and mismatched["replaced"] == 0, mismatched
        assert len(mismatched["skipped"]) == 1, mismatched
        assert "hydrated result differs" in mismatched["skipped"][0]["reason"]
        assert benchmark.json_load(promoted_record)["result_sha256"] == (
            stored["result_sha256"]
        )

    print("certified benchmark cache contract: ok")


if __name__ == "__main__":
    main()
