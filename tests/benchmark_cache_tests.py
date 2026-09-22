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
        "worker_mode": "packet-only",
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
        "read_paths": [],
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
            read=[],
            write=str(root / "work/agents" / agent_id),
            output=[str(result_path)],
            validate=validator,
            network=True,
            depth=1,
            section=[],
            requirement_id=unit["requirement_ids"],
            language=["Python"],
            worker_mode="packet-only",
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
            "primary_complete": True,
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


def main() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        unit, task = create_cacheable_task(root)
        freeze_manifest(root, unit)

        original = benchmark.cache_fingerprint(root, unit, task)
        assert original is not None
        original_fingerprint = original[0]
        assert benchmark.cache_eligible_unit(root, unit)

        # Quidra can never be certified/reused.
        quidra = {**unit, "assigned_languages": ["Quidra"]}
        assert not benchmark.cache_eligible_unit(root, quidra)

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

    print("certified benchmark cache contract: ok")


if __name__ == "__main__":
    main()
