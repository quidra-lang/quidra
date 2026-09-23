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
            "primary_config": benchmark.sha256_file(
                root / "template/config/primary.json"
            ),
            "benchmark_metadata": benchmark.sha256_file(
                root / "template/config/benchmark_metadata.json"
            ),
            "evaluation_spec": benchmark.sha256_file(
                root / "template/methodology/ecosystem.md"
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
    result = {
        "schema_version": 1,
        "evaluation": "ecosystem",
        "requirements": {
            "metric.documentation_quality": {"Python": 88.0}
        },
        "evidence": {"certified": "synthetic cache contract"},
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
    assert problem(fitted, trial_unit) is None, "a trial that never met its cap was refused"
    assert problem(cut_off, same_cap) is None, "the same cap is the same experiment"
    assert "cut off" in str(problem(cut_off, trial_unit)), "a truncated trial was reused"
    assert "above the current cap" in str(problem(oversized, trial_unit)), (
        "a completion larger than the current cap was reused"
    )
    assert "cache-annotate-caps" in str(problem(legacy, trial_unit)), (
        "a record without evidence was reused"
    )
    assert problem(legacy, unit) is None, "a packet-only record was held to trial evidence"
    assert problem(legacy, {**trial_unit, "max_output_tokens_per_call": 0}) is None

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
    assert "cut off" in str(problem(annotated, trial_unit))


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
    assert sc_payload["cache_epoch"] == "2026-09-medium"
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
    assert "micro_raw.json" in record["certification"]["raw_evidence_sha256"]
    assert record["result"] == result

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


def main() -> None:
    assert_accepted_trial_start_marks_the_scored_boundary()
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
        assert benchmark.cache_epoch(root, "ecosystem") == "2026-09"
        assert benchmark.cache_epoch(root, "semantic_compression") == "2026-09-medium"
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
        assert result["requirements"]["metric.documentation_quality"] == {"Python": 88.0}
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
                        "scores": {"Python": 88.0},
                        "ranking": [{"language": "Python", "score": 88.0}],
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
