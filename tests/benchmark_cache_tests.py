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
micro_measure = load("benchmark_cache_micro_measure", SCRIPTS / "micro_measure.py")


def make_workspace(tmp: Path) -> Path:
    root = tmp / "workspace"
    shutil.copytree(ROOT / "benchmark" / "template", root / "template")
    shutil.copy2(ROOT / "benchmark" / "config.json", root / "benchmark_config.json")
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

    policy = benchmark.json_load(root / "template/config/cache_policy.json")
    quidra_paths = benchmark.quidra_execution_input_paths(policy)
    quidra_objects = {path: "0" * 40 for path in quidra_paths}
    quidra_identity = {
        "schema_version": 1,
        "sha256": benchmark.sha256_bytes(
            json.dumps(
                quidra_objects, sort_keys=True, separators=(",", ":")
            ).encode("utf-8")
        ),
        "git_objects": quidra_objects,
    }

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
            "quidra_execution_identity": quidra_identity,
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
    rel = benchmark.cache_record_relative(unit, fingerprint, payload)
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

    # A validator-only cache-key migration can leave an identical paid result
    # under a second provenance record whose retained workspace lacks the
    # original trace. Inherit cap evidence only from a direct trace-backed
    # record with the exact same scientific payload, prompt and result.
    migrated_run = "migration-run"
    inherited_result = {"x": 2}
    inherited_result_raw = json.dumps(
        inherited_result, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    inherited_result_sha = benchmark.sha256_bytes(inherited_result_raw)
    inherited_prompt = "d" * 64
    scientific_payload = {
        "schema_version": 1,
        "evaluation": "llm_learnability",
        "work_unit_id": "learnability-i1-i2--python",
        "exact_task_packet_sha256": inherited_prompt,
        "model": "claude-sonnet-5",
        "provider": "anthropic-messages",
        "toolchains": {"Python": "3.12.3"},
    }
    donor_path = record_path.with_name("c" * 64 + ".json")
    inherited_path = record_path.with_name("d" * 64 + ".json")
    mismatch_path = record_path.with_name("e" * 64 + ".json")
    donor_record = {
        "schema_version": 1,
        "fingerprint": "c" * 64,
        "fingerprint_payload": {
            **scientific_payload,
            "validator_contract": "legacy runner spelling",
        },
        "evaluation": "llm_learnability",
        "result": inherited_result,
        "result_sha256": inherited_result_sha,
        "certification": {
            "learnability_integrity": True,
            "validator_pass": True,
            "unit_complete": True,
            "primary_complete": False,
            "scored_output_cap": 4000,
            "trial_calls": 6,
            "cap_truncated_trial_calls": 0,
            "max_trial_output_tokens": 188,
            "cap_evidence_source": "agent_trace.json retained by donor-run",
        },
        "provenance": {
            "run_id": "donor-run",
            "work_unit_id": "learnability-i1-i2--python",
            "prompt_sha256": inherited_prompt,
        },
    }
    inherited_record = {
        **donor_record,
        "fingerprint": "d" * 64,
        "fingerprint_payload": scientific_payload,
        "certification": {
            "learnability_integrity": True,
            "validator_pass": True,
            "unit_complete": True,
            "primary_complete": False,
        },
        "provenance": {
            "run_id": migrated_run,
            "work_unit_id": "learnability-i1-i2--python",
            "prompt_sha256": inherited_prompt,
        },
    }
    mismatch_record = json.loads(json.dumps(inherited_record))
    mismatch_record["fingerprint"] = "e" * 64
    mismatch_record["fingerprint_payload"]["model"] = "different-model"
    benchmark.json_dump(donor_path, donor_record)
    benchmark.json_dump(inherited_path, inherited_record)
    benchmark.json_dump(mismatch_path, mismatch_record)
    benchmark.json_dump(
        evidence / "run.json", {"schema_version": 1, "run_id": migrated_run}
    )
    benchmark.json_dump(evidence / "work" / "root" / "manifest.json", {
        "schema_version": 1,
        "work_units": [{
            "id": "learnability-i1-i2--python",
            "evaluation": "llm_learnability",
            "assigned_agent_id": "worker-without-retained-trace",
            "max_output_tokens_per_call": 4000,
        }],
    })
    inherited_summary = benchmark.annotate_cache_cap_evidence(source, evidence)
    inherited = benchmark.json_load(inherited_path)
    assert inherited["certification"]["scored_output_cap"] == 4000
    assert inherited["certification"]["cap_truncated_trial_calls"] == 0
    assert inherited["certification"]["max_trial_output_tokens"] == 188
    assert inherited["certification"]["cap_evidence_inherited_from"].endswith(
        "c" * 64 + ".json"
    )
    assert any(
        row.get("work_unit_id") == "learnability-i1-i2--python"
        and row.get("cap_evidence_inherited_from")
        for row in inherited_summary["annotated"]
    ), inherited_summary
    assert "scored_output_cap" not in (
        benchmark.json_load(mismatch_path).get("certification") or {}
    ), "scientifically different records must not inherit cap evidence"


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
    audit_task = dict(
        task,
        read_paths=[str(root / "template/programs/python/micro")],
    )
    pair = benchmark.cache_fingerprint(root, audit, audit_task)
    assert pair is not None, "an audit produced no cache key"
    fingerprint, payload = pair
    assert payload["toolchains"] == {"Python": "3.14.5"} and payload["result_kind"] == "audit"
    assert payload["reuse_audit_for"] == ["micro-python"]
    assert payload["semantic_evidence_contract"] == "language-quality-reuse-audit-v1"
    assert set(payload["readable_input_content_hashes"]) == {
        "template/programs/python/micro"
    }
    assert all(
        key not in payload
        for key in (
            "provider", "model", "frozen_sampling", "exact_task_packet_sha256",
            "worker_mode", "network_allowed", "validator_contract",
        )
    ), payload
    assert benchmark.cache_scope(audit) == "audit-micro-python"

    run = benchmark.json_load(root / "run.json")
    original_model = run["inference_identity"]["model"]
    run["inference_identity"]["model"] = "different-model"
    benchmark.json_dump(root / "run.json", run)
    assert benchmark.cache_fingerprint(root, audit, audit_task)[0] == fingerprint
    run["inference_identity"]["model"] = original_model
    benchmark.json_dump(root / "run.json", run)
    changed_task = dict(audit_task, prompt_sha256="f" * 64)
    assert benchmark.cache_fingerprint(root, audit, changed_task)[0] == fingerprint

    sc_unit = dict(audit, id="sc-probe--python", evaluation="semantic_compression",
                   phase="measurement", result_kind="requirements", assigned_languages=["Python"],
                   reuse_audit_for=[], requirement_ids=["metric.semantic_density"])
    sc_payload = benchmark.cache_fingerprint(root, sc_unit, task)[1]
    assert sc_payload["frozen_sampling"]["effort"] == "medium", sc_payload["frozen_sampling"]
    assert sc_payload["frozen_sampling"]["effort_source"] == "evaluation_effort"
    assert sc_payload["cache_epoch"] == "2026-09-canonical-fragments-v6-fixed-stdout"

    changed = dict(audit, input_hashes={"artifact_git_object": "b" * 40})
    assert benchmark.cache_fingerprint(root, changed, audit_task)[0] != fingerprint, (
        "a changed artifact must change the audit's key"
    )
    toolchains = benchmark.json_load(root / "results/toolchains.json")
    toolchains["toolchains"]["Python"]["canonical"] = "3.15.0"
    benchmark.json_dump(root / "results/toolchains.json", toolchains)
    assert benchmark.cache_fingerprint(root, audit, audit_task)[0] != fingerprint, (
        "a bumped toolchain must change the audit's key"
    )
    toolchains["toolchains"]["Python"]["canonical"] = "3.14.5"
    benchmark.json_dump(root / "results/toolchains.json", toolchains)
    assert benchmark.cache_fingerprint(root, audit, audit_task)[0] == fingerprint

    orphan = dict(audit, reuse_audit_for=["micro-rust"])
    assert benchmark.cache_fingerprint(root, orphan, audit_task) is None


def assert_adversarial_mechanical_language_shards_are_independent() -> None:
    """A Quidra change must not invalidate a comparison-language safety shard."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        toolchains = benchmark.json_load(root / "results/toolchains.json")
        toolchains["toolchains"]["Rust"] = {
            "canonical": "1.95.0",
            "raw": ["rustc 1.95.0"],
            "commands": [["rustc", "--version"]],
        }
        benchmark.json_dump(root / "results/toolchains.json", toolchains)
        rust_programs = root / "template/programs/rust/adversarial"
        assert rust_programs.is_dir()
        result_path = (
            root / "work/root/commands/lq-adversarial-mechanical--rust/result.json"
        )
        unit = {
            "id": "lq-adversarial-mechanical--rust",
            "evaluation": "language_quality",
            "phase": "measurement",
            "execution_kind": "command",
            "result_kind": "requirements",
            "runner_action": "adversarial-measure",
            "requirement_ids": ["metric.type_safety"],
            "dependencies": [],
            "read_paths": [
                str(rust_programs),
                str(root / "template/methodology-assets/language_quality"),
                str(root / "template/methodology-assets/scoring"),
            ],
            "evidence_paths": [str(result_path)],
            "network_allowed": False,
            "max_attempts": 1,
            "worker_mode": "runner-command",
            "assigned_languages": ["Rust"],
            "input_hashes": {
                "benchmark_metadata": benchmark.sha256_file(
                    root / "template/config/benchmark_metadata.json"
                ),
                "evaluation_spec_sections": benchmark.evaluation_spec_projection_sha256(
                    root, "language_quality", []
                ),
                "primary_config": benchmark.primary_config_projection_sha256(
                    root, "language_quality"
                ),
            },
        }
        task = benchmark.mechanical_task(unit)
        pair = benchmark.cache_fingerprint(root, unit, task)
        assert pair is not None
        fingerprint, payload = pair
        assert payload["assigned_languages"] == ["Rust"], payload
        assert set(payload["toolchains"]) == {"Rust"}, payload
        assert set(payload["runtime_toolchain_pins"]) == {"Rust"}, payload
        assert "quidra_target" not in payload

        run = benchmark.json_load(root / "run.json")
        changed = json.loads(json.dumps(
            run["evaluated"]["quidra_execution_identity"]
        ))
        changed["sha256"] = "f" * 64
        run["evaluated"]["quidra_execution_identity"] = changed
        benchmark.json_dump(root / "run.json", run)
        assert benchmark.cache_fingerprint(root, unit, task)[0] == fingerprint

        changed_path = next(rust_programs.glob("*"))
        original = changed_path.read_bytes()
        changed_path.write_bytes(original + b"\n")
        assert benchmark.cache_fingerprint(root, unit, task)[0] != fingerprint


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
        '[project]\nname = "Quidra"\nversion = "0.3.0"\nlanguage_version = "0.2"\n', encoding="utf-8"
    )
    result_path = root / "work" / "root" / "commands" / "lq-micro-mechanical" / "result.json"
    unit = {
        "id": "lq-micro-mechanical", "evaluation": "language_quality", "phase": "measurement",
        "execution_kind": "command", "result_kind": "requirements", "runner_action": "micro-measure",
        "requirement_ids": ["metric.native_execution_performance"], "dependencies": [],
        "read_paths": ["repo/tests/benchmark/quidra", "template/workloads"],
        "evidence_paths": [str(result_path)], "network_allowed": False, "max_attempts": 3,
        "worker_mode": "runner-command", "assigned_languages": [],
        "input_hashes": {
            "benchmark_metadata": benchmark.sha256_file(
                root / "template/config/benchmark_metadata.json"
            ),
            "evaluation_spec_sections": benchmark.evaluation_spec_projection_sha256(
                root, "language_quality", []
            ),
            "primary_config": benchmark.primary_config_projection_sha256(
                root, "language_quality"
            ),
        },
    }
    assert benchmark.cache_eligible_unit(root, unit), "a mechanical measurement is not cacheable"
    assert benchmark.cache_scope(unit) == "mechanical-micro-measure"
    task = benchmark.mechanical_task(unit)
    pair = benchmark.cache_fingerprint(root, unit, task)
    assert pair is not None, "a mechanical unit produced no cache key"
    fingerprint, payload = pair
    assert set(payload["toolchains"]) == set(languages) - {"Quidra"}, payload["toolchains"]
    assert payload["quidra_target"] == {"version": "0.3.0", "language_version": "0.2"}
    assert set(payload["measurement_script_hashes"]) == {"micro_measure.py"}
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
    rel = benchmark.cache_record_relative(unit, fingerprint, payload)
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

    # Same declared Quidra version is an immutable generation. Even when its
    # implementation/readable source changes and therefore the exact cache key
    # changes, the archived generation must hydrate instead of being measured
    # again. This is the concrete "same version => skip Quidra" contract.
    # Version history is now derived from the ordinary generation cache, not
    # from a parallel version_history store. Seed one complete synthetic Quidra
    # generation across all five Primary evaluations so the same-version
    # compatibility lookup sees the production shape.
    for evaluation in benchmark.PRIMARY_NAMES:
        benchmark.json_dump(
            benchmark.generation_file(
                root / "cache", evaluation, "quidra_v0.3.0"
            ),
            {
                "schema_version": 1,
                "language": "Quidra",
                "version": "0.3.0",
                "generation_id": "quidra_v0.3.0",
                "language_id": "quidra_v0.3.0",
                "version_source": "project.toml:[project].version",
                "evaluation": evaluation,
                "source_run_id": "synthetic-seed",
                "source_commit_sha": "0" * 40,
                "primary_score": 50.0,
                "normalized_metric_scores": {},
                "normalization_raw": {},
            },
        )
    (programs / "mb00.qui").write_text("print(2)\n", encoding="utf-8")
    changed_same_version = benchmark.cache_fingerprint(root, unit, task)
    assert changed_same_version is not None
    assert changed_same_version[0] != fingerprint
    shutil.rmtree(result_path.parent)
    freeze_manifest(root, unit)
    assert benchmark.hydrate_certified_cache(root, "language_quality") == 1
    same_receipt = benchmark.json_load(result_path.parent / "cache_receipt.json")
    assert same_receipt["reuse_mode"] == "quidra_same_version", same_receipt
    same_status = benchmark.json_load(root / "results/cache_status.json")
    assert same_status["hits"][unit["id"]]["reuse_mode"] == "quidra_same_version"

    # A bumped project.toml version is a new generation and must not use the
    # same-version fallback. It remains PENDING for a fresh measurement.
    (root / "repo" / "project.toml").write_text(
        '[project]\nname = "Quidra"\nversion = "0.4.0"\nlanguage_version = "0.2"\n',
        encoding="utf-8",
    )
    shutil.rmtree(result_path.parent)
    freeze_manifest(root, unit)
    assert benchmark.hydrate_certified_cache(root, "language_quality") == 0
    bumped_ledger = benchmark.json_load(root / "work/root/ledger.json")
    assert bumped_ledger["units"][unit["id"]]["status"] == "PENDING"
    (root / "repo" / "project.toml").write_text(
        '[project]\nname = "Quidra"\nversion = "0.3.0"\nlanguage_version = "0.2"\n',
        encoding="utf-8",
    )
    (programs / "mb00.qui").write_text("print(1)\n", encoding="utf-8")

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
    shutil.rmtree(result_path.parent, ignore_errors=True)
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


def assert_proficiency_cache_uses_runtime_prompt_identity_not_packet_wrapper() -> None:
    """Outer Task-Packet serialization is not the scored Proficiency experiment."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, task = create_cacheable_task(root)
        unit.update({
            "id": "proficiency-trials--python",
            "evaluation": "llm_proficiency",
            "assigned_languages": ["Python"],
            "requirement_ids": ["metric.correct_at_1"],
            "input_hashes": {
                "primary_config": benchmark.primary_config_projection_sha256(
                    root, "llm_proficiency"
                ),
                "benchmark_metadata": benchmark.sha256_file(
                    root / "template/config/benchmark_metadata.json"
                ),
                "evaluation_spec_sections": benchmark.evaluation_spec_projection_sha256(
                    root, "llm_proficiency", []
                ),
            },
            "network_allowed": False,
            "max_output_tokens_per_call": 16384,
        })
        pair = benchmark.cache_fingerprint(root, unit, task)
        assert pair is not None
        fingerprint, payload = pair
        assert "exact_task_packet_sha256" not in payload, payload
        assert payload["semantic_evidence_contract"] == (
            "llm-proficiency-runtime-owned-trials-v1"
        )
        assert payload["proficiency_primary_trial_set_sha256"] == (
            benchmark.proficiency_primary_trial_set_sha256(root)
        )
        assert payload["proficiency_primary_prompt_set_sha256"] == (
            benchmark.proficiency_primary_prompt_set_sha256(root, "Python")
        )

        # Simulate a JSON/TXT-style outer packet reserialization. The runtime
        # still sends the exact same scored prompts, so the scientific key stays.
        repacked = dict(task)
        repacked["prompt_sha256"] = "f" * 64
        assert benchmark.cache_fingerprint(root, unit, repacked)[0] == fingerprint

        # But a change to what the evaluated model actually sees must re-key.
        environment_path = root / "template/environment/environment.json"
        environment_bytes = environment_path.read_bytes()
        environment = benchmark.json_load(environment_path)
        environment["frozen_toolchain_recipes"]["Python"]["run"] = (
            "python3 -B FILE.py"
        )
        benchmark.json_dump(environment_path, environment)
        assert benchmark.cache_fingerprint(root, unit, repacked)[0] != fingerprint
        environment_path.write_bytes(environment_bytes)



def assert_execution_identity_paths_are_policy_authoritative() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        policy = benchmark.cache_policy(root)
        paths = benchmark.quidra_execution_input_paths(policy)
        current = benchmark.current_quidra_execution_identity(root)
        assert current is not None
        assert set(paths) == set(current["git_objects"]), (paths, current)

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
            "provenance": {"run_id": "canonical-test"},
        }
        assert benchmark.cache_quidra_execution_reuse_problem(root, record) is None

        run = benchmark.json_load(root / "run.json")
        changed = json.loads(json.dumps(current))
        first_path = next(iter(changed["git_objects"]))
        changed["git_objects"][first_path] = "f" * 40
        changed["sha256"] = "e" * 64
        run["evaluated"]["quidra_execution_identity"] = changed
        benchmark.json_dump(root / "run.json", run)
        problem = benchmark.cache_quidra_execution_reuse_problem(root, record)
        assert problem is not None and "implementation changed" in problem, problem

        run["evaluated"]["quidra_execution_identity"] = current
        benchmark.json_dump(root / "run.json", run)
        missing_identity = {
            "fingerprint_payload": {"assigned_languages": ["Quidra"]},
            "provenance": {"run_id": "canonical-test"},
        }
        problem = benchmark.cache_quidra_execution_reuse_problem(
            root, missing_identity
        )
        assert problem is not None and "no explicit execution identity" in problem, problem

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

        # Ecosystem uses the same version-generation rule as every other
        # evaluation. Calendar/date drift is not a cache-generation trigger.
        config_path = root / "benchmark_config.json"
        config_bytes = config_path.read_bytes()
        before = benchmark.language_generation(root, "Python")
        config = benchmark.json_load(config_path)
        config["languages"]["Python"]["version"] = "3.13.0"
        benchmark.json_dump(config_path, config)
        after = benchmark.language_generation(root, "Python")
        assert before["generation_id"] == "python_v3.12.3", before
        assert after["generation_id"] == "python_v3.13.0", after
        assert after["generation_id"] != before["generation_id"]
        # The evidence snapshot remains frozen by the rubric itself; changing
        # language generation does not invent a date epoch.
        benchmark.apply_ecosystem_runner_scores(root, task, result)
        config_path.write_bytes(config_bytes)

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
        sandbox_runtime = benchmark.json_load(
            root / "template/config/sandbox_agent.json"
        )
        max_turns = int(sandbox_runtime["max_turns"])
        assert row["output_tokens_per_call"] == int(
            sandbox_runtime["orchestration_max_output_tokens"]
        ), row
        assert row["scored_output_tokens_per_call"] == int(
            unit["max_output_tokens_per_call"]
        ), row
        assert row["orchestration_output_tokens_per_call"] == int(
            sandbox_runtime["orchestration_max_output_tokens"]
        ), row
        assert row["scored_calls_estimate"] == int(unit["max_llm_calls"]), row
        assert row["orchestration_calls_estimate"] == min(3, max_turns), row
        # The legacy flattened estimator priced the scored call at the larger
        # orchestration cap. The execution plan must now price the two call
        # classes separately without changing either call budget.
        pricing = benchmark.gateway_config(root)["anthropic_pricing"][
            "claude-sonnet-5"
        ]
        flattened_per_call = (
            row["input_tokens_per_call"]
            * float(pricing["input_usd_per_million_tokens"])
            + row["output_tokens_per_call"]
            * float(pricing["output_usd_per_million_tokens"])
        ) / 1_000_000.0
        assert row["estimated_uncached_usd"] < (
            row["planned_calls_estimate"] * flattened_per_call
        ), row
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


def assert_proficiency_budget_nominal_is_one_next_call_per_trial() -> None:
    """Proficiency nominal spend funds the next trial pass, not every possible repair."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, _ = create_cacheable_task(root)
        cfg = benchmark.json_load(root / "template/config/primary.json")[
            "llm_proficiency"
        ]
        trial_count = len(benchmark.proficiency_required_trial_ids(root))
        repair_factor = 1 + int(cfg["max_repair_turns"])
        unit.update({
            "id": "proficiency-trials--python",
            "evaluation": "llm_proficiency",
            "requirement_ids": ["metric.correct_at_1"],
            "max_llm_calls": trial_count * repair_factor,
            "max_output_tokens_per_call": 16384,
            "max_attempts": 5,
        })
        freeze_manifest(root, unit)

        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=1000.0,
            evaluation="llm_proficiency",
            safety_multiplier=1.25,
        )
        row = plan["units"][0]
        assert row["scored_calls_estimate"] == trial_count, row
        assert row["scored_calls_remaining_allowance"] == trial_count * repair_factor, row
        assert row["scored_calls_upper_bound"] > row["scored_calls_estimate"], row
        assert row["estimated_uncached_usd"] < row["retry_ceiling_uncached_usd"], row

        # A paid successful trial needs no nominal repurchase. An unresolved
        # paid trial reserves only its next repair, not all remaining repairs.
        # Proficiency completion state is runner-owned: budget planning must use
        # the trusted verification record, not the worker-visible summary (which
        # intentionally omits hidden score verdicts such as test_passed).
        trials = (
            root / "work/agents" / unit["assigned_agent_id"] / "trials"
        )
        trusted_root = (
            root / "work/root/proficiency-verification"
            / unit["assigned_agent_id"]
        )
        trial_manifest = benchmark.proficiency_trial_manifest(root)
        contract_sha = benchmark.proficiency_workload_contract_sha256(root)
        ids = benchmark.proficiency_required_trial_ids(root)
        for trial_id, passed in ((ids[0], True), (ids[1], False)):
            completion_sha = benchmark.sha256_bytes(
                f"{trial_id}-completion".encode("utf-8")
            )
            verification = {
                "schema_version": 2,
                "trial_id": trial_id,
                "language": "Python",
                "workload": trial_manifest[trial_id]["workload"],
                "source_sha256": completion_sha,
                "workload_contract_sha256": contract_sha,
                "compile_parse_ok": True,
                "oracle_tests": [{
                    "id": "public",
                    "hidden": False,
                    "passed": passed,
                }],
            }
            verification_path = (
                trusted_root / trial_id / "call_01" / "verification.json"
            )
            benchmark.json_dump(verification_path, verification)
            session_dir = trials / trial_id
            session_dir.mkdir(parents=True, exist_ok=True)
            benchmark.json_dump(
                session_dir / "session.json",
                {
                    "schema_version": 1,
                    "trial_id": trial_id,
                    "calls": [{
                        "completion_sha256": completion_sha,
                        "verification": benchmark.proficiency_verification_summary(
                            verification
                        ),
                        "verification_path": verification_path.relative_to(root).as_posix(),
                    }],
                },
            )
        assert production.nominal_sandbox_scored_calls(
            root, unit, trial_count * repair_factor, 2
        ) == trial_count - 1

        # Cancellation edge: the paid response may be durably committed to the
        # trusted checkpoint before session.json is updated. Execution restores
        # that call on resume, so the budget planner must credit it too.
        trial_id = ids[1]
        completion = f"{trial_id}-completion-2"
        completion_sha = benchmark.sha256_bytes(completion.encode("utf-8"))
        verification = {
            "schema_version": 2,
            "trial_id": trial_id,
            "language": "Python",
            "workload": trial_manifest[trial_id]["workload"],
            "source_sha256": completion_sha,
            "workload_contract_sha256": contract_sha,
            "compile_parse_ok": True,
            "oracle_tests": [{
                "id": "public",
                "hidden": False,
                "passed": True,
            }],
        }
        verification_path = (
            trusted_root / trial_id / "call_02" / "verification.json"
        )
        benchmark.json_dump(verification_path, verification)
        prompt = "repair prompt"
        record = {
            "call": 2,
            "prompt": prompt,
            "completion": completion,
            "prompt_path": (
                Path("trials") / trial_id / "prompt_02.txt"
            ).as_posix(),
            "completion_path": (
                Path("trials") / trial_id / "completion_02.txt"
            ).as_posix(),
            "prompt_sha256": benchmark.sha256_bytes(prompt.encode("utf-8")),
            "completion_sha256": completion_sha,
            "verification": benchmark.proficiency_verification_summary(
                verification
            ),
            "verification_path": verification_path.relative_to(root).as_posix(),
        }
        checkpoint_path = (
            root / "work/root/trial-checkpoints"
            / unit["assigned_agent_id"] / trial_id / "call_02.json"
        )
        benchmark.json_dump(checkpoint_path, {
            "schema_version": 1,
            "kind": "paid-trial-call-checkpoint-v1",
            "agent_id": unit["assigned_agent_id"],
            "evaluation": "llm_proficiency",
            "trial_id": trial_id,
            "call": 2,
            "record": record,
        })
        assert production.nominal_sandbox_scored_calls(
            root, unit, trial_count * repair_factor, 3
        ) == trial_count - 2


def assert_learnability_budget_nominal_is_one_next_call_per_trial() -> None:
    """Learnability nominal spend funds Primary calls, not every repair ceiling."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, _ = create_cacheable_task(root)
        cfg = benchmark.json_load(root / "template/config/primary.json")[
            "llm_learnability"
        ]
        primary_trials = (
            int(cfg["keyword_anonymization_seeds"])
            + int(cfg["vocabulary_anonymization_seeds"])
        )
        repair_factor = 1 + int(cfg["max_repair_turns"])
        unit.update({
            "id": "learnability-i1-i2--python",
            "evaluation": "llm_learnability",
            "requirement_ids": [
                "condition.i1_keyword_anonymization",
                "condition.i2_vocabulary_anonymization",
            ],
            "max_llm_calls": primary_trials * repair_factor,
            "max_output_tokens_per_call": 16384,
            "max_attempts": 5,
        })
        freeze_manifest(root, unit)

        plan = production.build_budget_plan(
            root,
            "claude-sonnet-5",
            available_usd=1000.0,
            evaluation="llm_learnability",
            safety_multiplier=1.25,
        )
        row = plan["units"][0]
        assert row["scored_calls_estimate"] == primary_trials, row
        assert row["scored_calls_remaining_allowance"] == (
            primary_trials * repair_factor
        ), row
        assert row["scored_calls_upper_bound"] > row["scored_calls_estimate"], row

        # A started Learnability trial has no runtime-owned pass verdict, so a
        # partial-run budget conservatively reserves one next call for it. Once
        # its full repair allowance is exhausted it needs no additional call.
        trial_dir = (
            root / "work/agents" / unit["assigned_agent_id"]
            / "trials/i1-t1"
        )
        trial_dir.mkdir(parents=True, exist_ok=True)
        benchmark.json_dump(
            trial_dir / "session.json",
            {
                "schema_version": 1,
                "trial_id": "i1-t1",
                "calls": [{"call": i + 1} for i in range(repair_factor)],
            },
        )
        assert production.nominal_sandbox_scored_calls(
            root, unit, primary_trials * repair_factor, repair_factor
        ) == primary_trials - 1


def assert_empty_cache_impact_is_a_valid_first_run() -> None:
    """A repository with no v1 records still produces a zero-impact plan."""
    with tempfile.TemporaryDirectory() as td:
        source = Path(td) / "source"
        source.mkdir()
        summary = benchmark.cache_impact(source)
        assert summary["valid"] == 0, summary
        assert summary["invalid"] == [], summary


def assert_repository_cache_impact_is_well_formed() -> None:
    """Repository cache impact is deterministic and internally consistent."""
    summary = benchmark.cache_impact(ROOT)
    assert isinstance(summary.get("valid"), int), summary
    assert isinstance(summary.get("invalid"), list), summary
    assert isinstance(summary.get("superseded"), list), summary
    assert summary.get("valid", 0) >= 0, summary
    assert summary.get("superseded_count") == len(summary["superseded"]), summary

def assert_evaluation_scoped_primary_cache() -> None:
    """Only the current evaluation's Primary settings may re-key that evaluation."""
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
            "non-scientific Language Quality bookkeeping re-keyed the evaluation"
        )
        primary_path.write_bytes(original_primary)

        primary = benchmark.json_load(primary_path)
        primary["llm_proficiency"]["primary_prompt_variants"].append(
            "cache-scope-regression"
        )
        benchmark.json_dump(primary_path, primary)
        assert benchmark.primary_config_projection_sha256(
            root, "ecosystem"
        ) == ecosystem_before, "a Proficiency-only setting re-keyed Ecosystem"
        assert benchmark.primary_config_projection_sha256(
            root, "llm_proficiency"
        ) != proficiency_before, "a Proficiency setting failed to re-key Proficiency"
        primary_path.write_bytes(original_primary)


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

        # Cross-evaluation state may coexist in the workspace/cache. A scoped
        # execution plan must never report those rows as part of this run.
        status = benchmark.json_load(root / "results/cache_status.json")
        status["hits"]["unrelated-proficiency"] = {
            "fingerprint": "a" * 64,
            "scope": "python",
            "record": "v1/llm-proficiency/python/unrelated.json",
        }
        status["misses"]["unrelated-proficiency"] = {
            "fingerprint": "b" * 64,
            "scope": "python",
            "reason": "no certified record",
        }
        status["invalidated"]["unrelated-proficiency"] = {
            "fingerprint": "b" * 64,
            "scope": "python",
            "reason": "unrelated prompt changed",
        }
        benchmark.json_dump(root / "results/cache_status.json", status)
        benchmark.json_dump(
            root / "results/cache_impact.json",
            {
                "schema_version": 1,
                "valid": 0,
                "invalid": [{
                    "record": "benchmark/cache/v1/llm-proficiency/python/unrelated.json",
                    "work_unit_id": "unrelated-proficiency",
                    "evaluation": "llm_proficiency",
                    "changed": ["exact_task_packet_sha256"],
                }],
            },
        )
        benchmark.json_dump(
            root / "results/partial_paid_checkpoint_status.json",
            {
                "schema_version": 1,
                "imported_units": [{
                    "work_unit_id": "unrelated-proficiency",
                    "restored_paid_calls": 99,
                }],
                "restored_paid_calls": 99,
            },
        )

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
        assert plan["scope_work_unit_count"] == 1, plan
        assert plan["cache_hits"] == 0, plan
        assert plan["cache_misses"] == 1, plan
        assert plan["cache_invalidated_units"] == 0, plan
        assert plan["invalidated_cache_records"] == [], plan
        assert plan["partial_paid_restored_calls"] == 0, plan
        assert plan["partial_paid_resumed_units"] == [], plan

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
                    "call": 1,
                    "prompt": prompt,
                    "prompt_sha256": benchmark.sha256_bytes(prompt.encode()),
                    "completion": completion,
                    "completion_sha256": benchmark.sha256_bytes(completion.encode()),
                    "prompt_path": "trials/case-t1/prompt_01.txt",
                    "completion_path": "trials/case-t1/completion_01.txt",
                    "verification": None,
                    "verification_path": None,
                }],
            },
        )
        trusted_call_dir = (
            root / "work/root/trial-checkpoints" / agent_id / "case-t1"
        )
        benchmark.json_dump(
            trusted_call_dir / "call_01.json",
            {
                "schema_version": 1,
                "kind": "paid-trial-call-checkpoint-v1",
                "agent_id": agent_id,
                "evaluation": unit["evaluation"],
                "trial_id": "case-t1",
                "call": 1,
                "record": {
                    "call": 1,
                    "prompt": prompt,
                    "prompt_sha256": benchmark.sha256_bytes(prompt.encode()),
                    "prompt_path": "trials/case-t1/prompt_01.txt",
                    "completion": completion,
                    "completion_sha256": benchmark.sha256_bytes(completion.encode()),
                    "completion_path": "trials/case-t1/completion_01.txt",
                    "stop_reason": "end_turn",
                    "incomplete": None,
                    "usage": {"input_tokens": 10, "output_tokens": 10},
                    "verification": None,
                    "verification_path": None,
                },
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
        assert (
            root / "work/root/trial-checkpoints" / agent_id
            / "case-t1/call_01.json"
        ).is_file()
        assert (agent_dir / "resume_trace.json").is_file()

        # Re-exporting identical state must not generate a new cache save.
        again = benchmark.export_partial_paid_checkpoints(root, store)
        assert again["updated_unit_count"] == 0, again

        # The trusted checkpoint is self-contained and outside worker authority.
        # Lose every worker-side trial/session/journal file before export; the
        # already-paid call must still survive a true cross-run roundtrip.
        shutil.rmtree(agent_dir / "trials")
        (agent_dir / "trial_call_journal.json").unlink()
        for name in ("resume_trace.json", "agent_trace.partial.json", "agent_trace.json"):
            path = agent_dir / name
            if path.exists():
                path.unlink()

        trusted_only_store = base / "paid-state-trusted-only"
        trusted_only = benchmark.export_partial_paid_checkpoints(
            root, trusted_only_store
        )
        assert trusted_only["updated_unit_count"] == 1, trusted_only
        assert trusted_only["exported_units"][0]["paid_call_count"] == 1, trusted_only

        # Remove even the local trusted record, then restore it solely from the
        # private cross-run paid-state store.
        shutil.rmtree(root / "work/root/trial-checkpoints" / agent_id)
        restored_trusted_only = benchmark.import_partial_paid_checkpoints(
            root, trusted_only_store
        )
        assert restored_trusted_only["imported_unit_count"] == 1, restored_trusted_only
        assert restored_trusted_only["restored_paid_calls"] == 1, restored_trusted_only
        assert (
            root / "work/root/trial-checkpoints" / agent_id
            / "case-t1/call_01.json"
        ).is_file()
        assert not (agent_dir / "trial_call_journal.json").exists()
        assert not (agent_dir / "trials").exists()

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



def assert_learnability_worker_core_projection_is_nonsemantic_only() -> None:
    common = """# Benchmark Worker Core Rules

1. Keep the scored contract fixed.
2. Independent scored LLM trials must not share previous trial generations.
"""
    shared_tail = """
For reusable-artifact currency audits with no Primary requirement IDs:
keep the ordinary audit contract.
"""
    old = common + shared_tail
    current = common + """
### Semantic Compression support adjudication exception
For support adjudication only, return UTF-8 text leaves instead of result.json.

""" + shared_tail.lstrip()
    assert (
        benchmark._learnability_worker_core_projection(old)
        == benchmark._learnability_worker_core_projection(current)
    ), "SC-only JSON/TXT transport drift must not repurchase Learnability"

    # The retained paid Worker Core and the current file differ by this exact
    # seam: removing the SC-only appendix leaves one extra blank line before
    # the following shared paragraph. Formatting at that deletion boundary is
    # not a scientific Learnability change.
    current_with_extra_seam_newline = current.replace(
        "\nFor reusable-artifact currency audits",
        "\n\nFor reusable-artifact currency audits",
        1,
    )
    assert (
        benchmark._learnability_worker_core_projection(old)
        == benchmark._learnability_worker_core_projection(
            current_with_extra_seam_newline
        )
    ), "an extra seam newline after removing SC transport text caused a paid MISS"
    assert "reusable-artifact currency audits" in (
        benchmark._learnability_worker_core_projection(current)
    ), "the projection must preserve shared worker rules after the SC-only appendix"

    substantive = current.replace(
        "must not share previous trial generations",
        "may share previous trial generations",
    )
    assert (
        benchmark._learnability_worker_core_projection(old)
        != benchmark._learnability_worker_core_projection(substantive)
    ), "a Learnability-relevant worker rule change must remain a cache miss"

    old_components = [
        {"kind": "task", "sha256": "a" * 64},
        {"kind": "embedded:worker_core.md", "sha256": "b" * 64},
        {"kind": "embedded:benchmark_metadata.json", "sha256": "c" * 64},
    ]
    current_components = [
        {"kind": "task", "sha256": "a" * 64},
        {"kind": "embedded:worker_core.md", "sha256": "d" * 64},
        {"kind": "embedded:benchmark_metadata.json", "sha256": "c" * 64},
    ]
    assert benchmark._prompt_component_signature_without_scoped(
        old_components, "llm_learnability"
    ) == benchmark._prompt_component_signature_without_scoped(
        current_components, "llm_learnability"
    )
    assert benchmark._prompt_component_signature_without_scoped(
        old_components, "llm_proficiency"
    ) != benchmark._prompt_component_signature_without_scoped(
        current_components, "llm_proficiency"
    )


def assert_same_version_quidra_is_removed_from_mechanical_execution() -> None:
    """A peer-only cache miss must not execute an already archived Quidra generation."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        current_version = benchmark.project_version(ROOT)
        assert current_version
        (root / "repo" / "project.toml").write_text(
            f'[project]\nname = "Quidra"\nversion = "{current_version}"\nlanguage_version = "0.2"\n',
            encoding="utf-8",
        )
        requirements = [
            "metric.native_execution_performance",
            "metric.long_running_performance",
            "metric.compile_build_performance",
            "metric.startup_latency",
            "metric.memory_efficiency",
            "metric.runtime_overhead",
            "metric.source_code_size",
            "metric.binary_artifact_size",
        ]
        archived_scores = {
            requirement_id: float(50 + index)
            for index, requirement_id in enumerate(requirements)
        }
        (root / "version_history").mkdir(parents=True, exist_ok=True)
        benchmark.json_dump(
            root / "version_history" / "index.json",
            {
                "schema_version": 1,
                "version_ssot": "project.toml:[project].version",
                "versions": [{
                    "version": current_version,
                    "language_id": f"quidra_v{current_version}",
                    "source_run_id": "synthetic-version-history",
                    "source_commit_sha": "0" * 40,
                    "primary_scores": {"language_quality": 60.0},
                    "normalized_metric_scores": {
                        "language_quality": archived_scores,
                    },
                }],
            },
        )
        executed, archived = micro_measure.execution_languages_with_archived_quidra(
            root, requirements, list(micro_measure.LANGUAGES)
        )
        assert archived is not None, archived
        assert archived["version"] == current_version
        assert archived["scores"] == archived_scores
        assert "Quidra" not in executed
        assert executed == [lang for lang in micro_measure.LANGUAGES if lang != "Quidra"]

        # Missing even one required score is fail-safe: Quidra must execute.
        history = benchmark.json_load(root / "version_history" / "index.json")
        del history["versions"][0]["normalized_metric_scores"]["language_quality"][requirements[0]]
        benchmark.json_dump(root / "version_history" / "index.json", history)
        executed_missing, archived_missing = (
            micro_measure.execution_languages_with_archived_quidra(
                root, requirements, list(micro_measure.LANGUAGES)
            )
        )
        assert archived_missing is None
        assert executed_missing == list(micro_measure.LANGUAGES)

        # A version bump is a new generation: it must execute and later append.
        history["versions"][0]["normalized_metric_scores"]["language_quality"] = archived_scores
        benchmark.json_dump(root / "version_history" / "index.json", history)
        (root / "repo" / "project.toml").write_text(
            '[project]\nname = "Quidra"\nversion = "999.0.0"\nlanguage_version = "0.2"\n',
            encoding="utf-8",
        )
        executed_bumped, archived_bumped = (
            micro_measure.execution_languages_with_archived_quidra(
                root, requirements, list(micro_measure.LANGUAGES)
            )
        )
        assert archived_bumped is None
        assert executed_bumped == list(micro_measure.LANGUAGES)

        # Both scored mechanical paths must use the shared skip planner.
        micro_source = (SCRIPTS / "micro_measure.py").read_text(encoding="utf-8")
        adversarial_source = (SCRIPTS / "adversarial_measure.py").read_text(encoding="utf-8")
        assert micro_source.count("execution_languages_with_archived_quidra(") >= 2
        assert "mm.execution_languages_with_archived_quidra(" in adversarial_source

        # The Quidra pre-measurement audit is part of the same skip guarantee:
        # an archived generation must not build or execute Quidra merely to
        # unlock a mixed mechanical unit.
        (root / "repo" / "project.toml").write_text(
            f'[project]\nname = "Quidra"\nversion = "{current_version}"\nlanguage_version = "0.2"\n',
            encoding="utf-8",
        )
        audit_unit = {
            "id": "lq-quidra-audit",
            "runner_action": "quidra-audit",
        }
        original_manifest_unit = micro_measure.manifest_unit
        original_compiler = micro_measure.ensure_target_compiler
        try:
            micro_measure.manifest_unit = lambda _root, _id: audit_unit
            micro_measure.ensure_target_compiler = lambda _root: (_ for _ in ()).throw(
                AssertionError("same-version audit attempted to build Quidra")
            )
            assert micro_measure.audit(root, "lq-quidra-audit") == 0
        finally:
            micro_measure.manifest_unit = original_manifest_unit
            micro_measure.ensure_target_compiler = original_compiler
        audit_result = micro_measure.load_json(
            root / "work/root/commands/lq-quidra-audit/result.json"
        )
        assert audit_result["requirements"]["gate.quidra_programs_current"] is True
        assert audit_result["evidence"]["execution_skipped"] is True

        # A new version has no archived generation, so audit must not short-circuit.
        (root / "repo" / "project.toml").write_text(
            '[project]\nname = "Quidra"\nversion = "999.0.0"\nlanguage_version = "0.2"\n',
            encoding="utf-8",
        )
        assert micro_measure.archived_quidra_generation(root) is None



def assert_historical_generations_change_relative_normalization() -> None:
    # Language Quality: adding an older, faster generation must rescale the
    # current generations rather than replaying their old normalized scores.
    def lq_row(value: float) -> dict[str, object]:
        return {
            "workloads": {
                "mb01": {
                    "source_bytes": value,
                    "artifact_effective_bytes": value,
                    "compile_effective_seconds": value,
                    "cold_seconds": value,
                    "rss_bytes": value,
                    "steady_seconds": value,
                }
            },
            "startup_seconds": value,
            "startup_rss_bytes": value,
        }

    initial = benchmark.language_quality_generation_scores_from_raw({
        "python_v1": lq_row(2.0),
        "rust_v1": lq_row(1.0),
    })
    expanded = benchmark.language_quality_generation_scores_from_raw({
        "python_v1": lq_row(2.0),
        "rust_v1": lq_row(1.0),
        "rust_v0": lq_row(0.5),
    })
    assert initial["metric.native_execution_performance"]["rust_v1"] == 100.0
    assert expanded["metric.native_execution_performance"]["rust_v1"] == 50.0
    assert expanded["metric.native_execution_performance"]["python_v1"] == 25.0

    # Semantic Compression: the same rule applies to min-max metrics.
    directions = {
        metric: ("higher_is_better" if metric == "metric.semantic_density" else "lower_is_better")
        for metric in benchmark.SC_RELATIVE_METRICS
    }
    base_raw = {
        "python_v1": {metric: 2.0 for metric in benchmark.SC_RELATIVE_METRICS},
        "rust_v1": {metric: 1.0 for metric in benchmark.SC_RELATIVE_METRICS},
    }
    initial_sc = benchmark.semantic_compression_generation_scores_from_raw(
        base_raw, directions
    )
    expanded_raw = json.loads(json.dumps(base_raw))
    expanded_raw["rust_v0"] = {
        metric: (3.0 if metric == "metric.semantic_density" else 0.5)
        for metric in benchmark.SC_RELATIVE_METRICS
    }
    expanded_sc = benchmark.semantic_compression_generation_scores_from_raw(
        expanded_raw, directions
    )
    assert initial_sc["metric.semantic_density"]["python_v1"] == 100.0
    assert expanded_sc["metric.semantic_density"]["python_v1"] == 50.0
    assert expanded_sc["metric.semantic_determinacy"]["rust_v1"] != (
        initial_sc["metric.semantic_determinacy"]["rust_v1"]
    )


def assert_all_languages_use_version_generations() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        (root / "repo" / "project.toml").write_text(
            '[project]\nname = "Quidra"\nversion = "0.3.0"\nlanguage_version = "0.2"\n',
            encoding="utf-8",
        )
        languages = benchmark.metadata_languages(root)
        generations = benchmark.language_generations(root, languages)
        assert set(generations) == set(languages)
        assert generations["Quidra"]["generation_id"] == "quidra_v0.3.0"
        assert generations["Python"]["generation_id"] == "python_v3.12.3"

        config_path = root / "benchmark_config.json"
        config = benchmark.json_load(config_path)
        before = {
            language: row["generation_id"]
            for language, row in generations.items()
        }
        config["languages"]["Python"]["version"] = "9.9.9"
        benchmark.json_dump(config_path, config)
        after = {
            language: row["generation_id"]
            for language, row in benchmark.language_generations(root, languages).items()
        }
        assert after["Python"] == "python_v9.9.9"
        assert all(
            after[language] == before[language]
            for language in languages
            if language != "Python"
        ), (before, after)


def assert_generation_immutability_uses_raw_and_direct_metrics() -> None:
    lq_old = {
        "metric.native_execution_performance": 80.0,
        "metric.readability": 90.0,
    }
    lq_new = {
        "metric.native_execution_performance": 55.0,
        "metric.readability": 90.0,
    }
    assert benchmark.generation_immutable_metric_scores(
        "language_quality", lq_old
    ) == benchmark.generation_immutable_metric_scores(
        "language_quality", lq_new
    )
    assert benchmark.generation_immutable_metric_scores(
        "ecosystem", {"metric.documentation_quality": 90.0}
    ) != benchmark.generation_immutable_metric_scores(
        "ecosystem", {"metric.documentation_quality": 85.0}
    )

    sc_old = {
        "metric.semantic_density": 100.0,
        "metric.capability_coverage": 80.0,
    }
    sc_new = {
        "metric.semantic_density": 60.0,
        "metric.capability_coverage": 80.0,
    }
    assert benchmark.generation_immutable_metric_scores(
        "semantic_compression", sc_old
    ) == benchmark.generation_immutable_metric_scores(
        "semantic_compression", sc_new
    )


def main() -> None:
    assert_generation_immutability_uses_raw_and_direct_metrics()
    assert_historical_generations_change_relative_normalization()
    assert_all_languages_use_version_generations()
    assert_same_version_quidra_is_removed_from_mechanical_execution()
    assert_learnability_worker_core_projection_is_nonsemantic_only()
    assert_certified_checkpoint_promotes_prompt_dependencies()
    assert_partial_paid_checkpoint_roundtrip()
    assert_optional_units_do_not_enter_required_resume_set()
    assert_corrupt_cache_is_leaf_local_and_explicit()
    assert_execution_plan_classifies_cache_decisions()
    assert_packet_paid_response_commit_is_replayable()
    assert_empty_cache_impact_is_a_valid_first_run()
    assert_repository_cache_impact_is_well_formed()
    assert_evaluation_scoped_primary_cache()
    assert_budget_plan_excludes_complete_units()
    assert_budget_plan_exposes_configured_retry_ceiling()
    assert_proficiency_budget_nominal_is_one_next_call_per_trial()
    assert_learnability_budget_nominal_is_one_next_call_per_trial()
    assert_accepted_trial_start_marks_the_scored_boundary()
    assert_language_quality_design_runner_owned_scoring()
    assert_ecosystem_runner_owned_scoring()
    assert_execution_identity_paths_are_policy_authoritative()
    assert_quidra_execution_identity_reuse_guard()
    assert_cached_validator_rejection_becomes_miss()
    assert_proficiency_cache_requires_exact_primary_trial_set()
    assert_proficiency_cache_uses_runtime_prompt_identity_not_packet_wrapper()
    with tempfile.TemporaryDirectory() as mechanical_td:
        # Its own workspace: the test freezes a manifest of one mechanical unit.
        assert_mechanical_measurements_are_cacheable(make_workspace(Path(mechanical_td)), Path(mechanical_td))
    assert_adversarial_mechanical_language_shards_are_independent()
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

        # Runner validation is applied again on hydration and must never be a
        # paid-result dependency. Tightening/changing only the validator must
        # preserve the semantic cache key.
        changed_validator = {**unit, "validator_command": "python3 newer-validator.py"}
        assert benchmark.cache_fingerprint(root, changed_validator, task)[0] == original_fingerprint, (
            "runner validator changes must not invalidate paid semantic evidence"
        )

        # SC cohort adjudication has an explicit semantic-evidence key. Its
        # generated support input/rubric/model/sampling remain dependencies, but
        # transport and output-serialization instructions do not.
        support = {
            **unit,
            "id": "sc-support-adjudication--f04-p1",
            "evaluation": "semantic_compression",
            "assigned_languages": [],
            "requirement_ids": ["annotation.support_adjudication--f04-p1"],
            "worker_mode": "packet-only",
            "network_allowed": False,
        }
        support_task = dict(task)
        support_task["prompt_sha256"] = "1" * 64
        support_pair = benchmark.cache_fingerprint(root, support, support_task)
        assert support_pair is not None
        support_payload = support_pair[1]
        assert support_payload["semantic_evidence_contract"] == "sc-support-adjudication-v1"
        assert "exact_task_packet_sha256" not in support_payload
        assert "worker_mode" not in support_payload
        changed_transport_task = dict(support_task, prompt_sha256="2" * 64)
        changed_transport = {**support, "worker_mode": "sandbox-agent", "validator_command": "new validator"}
        assert benchmark.cache_fingerprint(root, changed_transport, changed_transport_task)[0] == support_pair[0], (
            "SC serialization/transport-only changes must not repurchase adjudication"
        )

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

        # Ecosystem is version-generation based, not calendar based. Merely
        # crossing a date/month boundary must not invalidate a language result.
        epoch_policy = benchmark.cache_policy(root)
        assert benchmark.cache_epoch(root, "ecosystem") == "stable"
        for evaluation in ("semantic_compression", "llm_proficiency"):
            assert benchmark.cache_epoch(root, evaluation) == (
                epoch_policy["declared_epochs"][evaluation]
            )
        assert benchmark.cache_epoch(root, "llm_learnability") == "stable"

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
