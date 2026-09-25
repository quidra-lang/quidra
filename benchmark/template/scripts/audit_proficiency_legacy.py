#!/usr/bin/env python3
"""Audit historical LLM Proficiency trials for current-contract recertification.

This tool is deliberately conservative. It never changes cache records and it
never calls a model. It reads preserved certified records plus extracted
workspace evidence, compares every historical trial with the current frozen
Proficiency contract, and emits trial-level A-E classifications. Only category
D trials are candidates for a separate current mechanical re-verification;
category E trials must be re-run with the provider.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
from typing import Any


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_benchmark(source_repo: Path):
    path = source_repo / "benchmark" / "template" / "scripts" / "benchmark.py"
    spec = importlib.util.spec_from_file_location("quidra_benchmark_recert_audit", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot import benchmark runner: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_evidence(values: list[str]) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for raw in values:
        if "=" not in raw:
            raise SystemExit(
                "--evidence must be RUN_ID=/path/to/extracted/workspace-evidence"
            )
        run_id, path = raw.split("=", 1)
        root = Path(path).resolve()
        if not (root / "run.json").is_file():
            raise SystemExit(f"evidence has no run.json: {root}")
        actual = str(load_json(root / "run.json").get("run_id") or "")
        if actual and actual != run_id:
            raise SystemExit(
                f"evidence run id mismatch: expected {run_id}, found {actual}"
            )
        out[run_id] = root
    return out


def record_language(record: dict[str, Any]) -> str | None:
    assigned = (
        (record.get("fingerprint_payload") or {}).get("assigned_languages") or []
    )
    if len(assigned) != 1:
        assigned = record.get("assigned_languages") or []
    return str(assigned[0]) if len(assigned) == 1 else None


def current_nontrajectory_compatibility(
    old: dict[str, Any], current: dict[str, Any]
) -> tuple[bool, list[str]]:
    """Compare scientific dependencies independent of the prompt trajectory."""
    fields = (
        "evaluation",
        "assigned_languages",
        "provider",
        "model",
        "frozen_sampling",
        "toolchains",
        "runtime_toolchain_pins",
        "worker_mode",
        "network_allowed",
        "quidra_target",
        "proficiency_workload_contract_sha256",
    )
    mismatches: list[str] = []
    for key in fields:
        if old.get(key) != current.get(key):
            mismatches.append(key)
    return not mismatches, mismatches


FORBIDDEN_REPAIR_PATTERNS = (
    re.compile(
        r"hidden[_ -]?(?:input|case|oracle|test|verdict|count|pass|fail|stdout|stderr|expected)",
        re.I,
    ),
    re.compile(
        r"(?:oracle|holdout|unseen)[_ -]?(?:input|expected|output|verdict|failure|result)",
        re.I,
    ),
)


def legacy_hidden_feedback_detected(prompt: str) -> bool:
    # Historical prompts predate the current fixed boilerplate. For migration,
    # any explicit hidden/holdout diagnostic is disqualifying. This is only an
    # extra guard: exact current repair-prompt reconstruction is still required
    # before a category-D candidate may ever be promoted.
    return any(pattern.search(prompt) for pattern in FORBIDDEN_REPAIR_PATTERNS)


def find_trace(evidence_root: Path, work_unit_id: str) -> Path | None:
    direct = (
        evidence_root
        / "work"
        / "agents"
        / f"worker-{work_unit_id}"
        / "agent_trace.json"
    )
    if direct.is_file():
        return direct
    matches = sorted(
        (evidence_root / "work" / "agents").glob(
            f"*{work_unit_id}*/agent_trace.json"
        )
    )
    return matches[0] if len(matches) == 1 else None


def audit_record(
    benchmark,
    source_repo: Path,
    current_root: Path,
    record_path: Path,
    record: dict[str, Any],
    evidence: dict[str, Path],
) -> dict[str, Any]:
    provenance = record.get("provenance") or {}
    run_id = str(provenance.get("run_id") or "")
    work_unit_id = str(provenance.get("work_unit_id") or "")
    language = record_language(record)
    row: dict[str, Any] = {
        "source_record": record_path.relative_to(source_repo).as_posix(),
        "source_record_sha256": benchmark.sha256_file(record_path),
        "source_fingerprint": record.get("fingerprint"),
        "source_run_id": run_id,
        "work_unit_id": work_unit_id,
        "language": language,
        "trials": [],
    }
    evroot = evidence.get(run_id)
    if evroot is None or language is None:
        row.update(
            {
                "classification": "E",
                "reason": "source evidence or language identity unavailable",
            }
        )
        return row
    trace_path = find_trace(evroot, work_unit_id)
    if trace_path is None:
        row.update(
            {
                "classification": "E",
                "reason": "preserved agent_trace.json unavailable",
            }
        )
        return row
    trace = load_json(trace_path)
    trials = ((trace.get("trials") or {}).get("trials") or {})
    if not isinstance(trials, dict) or not trials:
        row.update(
            {
                "classification": "E",
                "reason": "preserved trace contains no scored trials",
            }
        )
        return row

    current_manifest = benchmark.proficiency_trial_manifest(current_root)
    # A historical trial ID is only a label.  Re-keying/renaming a trial must
    # not make paid evidence scientifically different, so compare the exact
    # model-visible initial prompt against every current Primary trial for the
    # same language.  The old ID is retained only as audit metadata.
    current_prompt_to_trial_ids: dict[str, list[str]] = {}
    for current_trial_id in current_manifest:
        expected = benchmark.proficiency_expected_prompt(
            current_root, language, current_trial_id
        )
        current_prompt_to_trial_ids.setdefault(expected, []).append(
            current_trial_id
        )
    manifest = load_json(current_root / "work" / "root" / "manifest.json")
    unit = next(
        (
            u
            for u in manifest.get("work_units", [])
            if str(u.get("id")) == work_unit_id
        ),
        None,
    )
    current_payload = None
    if isinstance(unit, dict):
        task_path = (
            current_root
            / "work"
            / "agents"
            / str(unit["assigned_agent_id"])
            / "task.json"
        )
        if task_path.is_file():
            task = load_json(task_path)
            current_payload = benchmark.cache_fingerprint_payload(
                current_root, unit, task
            )
    old_payload = record.get("fingerprint_payload") or {}
    _deps_ok, dependency_mismatches = (
        current_nontrajectory_compatibility(old_payload, current_payload)
        if isinstance(current_payload, dict)
        else (False, ["current_work_unit"])
    )
    if language == "Quidra":
        target_problem = benchmark.cache_quidra_execution_reuse_problem(
            current_root, record
        )
        if target_problem:
            dependency_mismatches.append(
                "quidra_execution_identity:" + target_problem
            )

    reusable_candidates = 0
    hidden_feedback_trials = 0
    prompt_mismatch_trials = 0
    id_mismatch_trials = 0
    for trial_id, trial in sorted(trials.items()):
        calls = (trial or {}).get("calls") or []
        item: dict[str, Any] = {
            "trial_id": str(trial_id),
            "call_count": len(calls),
            "classification": "E",
            "reasons": [],
        }
        if str(trial_id) not in current_manifest:
            id_mismatch_trials += 1
        if (
            not calls
            or not isinstance(calls[0], dict)
            or not isinstance(calls[0].get("prompt"), str)
        ):
            item["reasons"].append("initial_prompt_unavailable")
        else:
            initial_prompt = calls[0]["prompt"]
            item["initial_prompt_sha256"] = benchmark.sha256_bytes(
                initial_prompt.encode("utf-8")
            )
            matched_current_ids = current_prompt_to_trial_ids.get(
                initial_prompt, []
            )
            if len(matched_current_ids) == 1:
                item["matched_current_trial_id"] = matched_current_ids[0]
            elif not matched_current_ids:
                item["reasons"].append(
                    "initial_prompt_not_byte_identical_to_any_current_trial"
                )
                prompt_mismatch_trials += 1
            else:
                # Current prompt identities are expected to be unique.  Never
                # guess which current cell an ambiguous historical prompt
                # belongs to.
                item["reasons"].append(
                    "initial_prompt_matches_multiple_current_trials"
                )
                prompt_mismatch_trials += 1
        leaked = any(
            isinstance(call, dict)
            and index > 0
            and isinstance(call.get("prompt"), str)
            and legacy_hidden_feedback_detected(call["prompt"])
            for index, call in enumerate(calls)
        )
        if leaked:
            item["reasons"].append(
                "model_visible_hidden_or_holdout_feedback_detected"
            )
            hidden_feedback_trials += 1
        if dependency_mismatches:
            item["reasons"].append(
                "nontrajectory_dependency_mismatch:"
                + ",".join(dependency_mismatches)
            )
        if not item["reasons"]:
            item["classification"] = "D"
            item["reasons"] = [
                "exact_contract_candidate_requires_current_mechanical_reverification"
            ]
            reusable_candidates += 1
        row["trials"].append(item)

    row["trial_count"] = len(row["trials"])
    row["candidate_trial_count"] = reusable_candidates
    row["hidden_feedback_detected_trial_count"] = hidden_feedback_trials
    row["prompt_mismatch_trial_count"] = prompt_mismatch_trials
    row["trial_id_mismatch_count"] = id_mismatch_trials
    row["dependency_mismatches"] = dependency_mismatches
    row["classification"] = (
        "D" if reusable_candidates == row["trial_count"] else "E"
    )
    row["reason"] = (
        "all trials exactly match the current input contract and require only current mechanical re-verification"
        if row["classification"] == "D"
        else "one or more trials change the scored experimental input/identity and cannot be recertified"
    )
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-repo", required=True)
    parser.add_argument("--workspace", required=True)
    parser.add_argument(
        "--evidence",
        action="append",
        default=[],
        help="RUN_ID=/extracted/workspace-evidence",
    )
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    source = Path(args.source_repo).resolve()
    root = Path(args.workspace).resolve()
    evidence = parse_evidence(args.evidence)
    benchmark = load_benchmark(source)
    current_epoch = benchmark.cache_epoch(root, "llm_proficiency")
    records: list[tuple[Path, dict[str, Any]]] = []
    for path in sorted(
        (
            source
            / "benchmark"
            / "cache"
            / "v1"
            / "llm-proficiency"
        ).glob("*/*.json")
    ):
        record = load_json(path)
        if str((record.get("fingerprint_payload") or {}).get("cache_epoch") or "") == current_epoch:
            continue
        records.append((path, record))
    audited = [
        audit_record(
            benchmark, source, root, path, record, evidence
        )
        for path, record in records
    ]
    trials = [
        trial
        for record in audited
        for trial in record.get("trials", [])
    ]
    summary = {
        "schema_version": 1,
        "migration_rule": "llm-proficiency-current-contract-trial-audit",
        "migration_rule_version": 1,
        "source_record_count": len(audited),
        "source_trial_count": len(trials),
        "category_D_candidate_trials": sum(
            1
            for trial in trials
            if trial.get("classification") == "D"
        ),
        "category_E_trials": sum(
            1
            for trial in trials
            if trial.get("classification") == "E"
        ),
        "hidden_feedback_detected_trials": sum(
            int(record.get("hidden_feedback_detected_trial_count", 0))
            for record in audited
        ),
        "current_certified_trial_count": sum(
            int((record.get("certification") or {}).get(
                "proficiency_primary_trial_count", 0
            ) or 0)
            for path in sorted(
                (
                    source
                    / "benchmark"
                    / "cache"
                    / "v1"
                    / "llm-proficiency"
                ).glob("*/*.json")
            )
            for record in [load_json(path)]
            if str((record.get("fingerprint_payload") or {}).get(
                "cache_epoch"
            ) or "") == current_epoch
        ),
        "note": (
            "D is only a candidate state. Promotion additionally requires deterministic "
            "replay with the current trusted verifier, current validator PASS, and "
            "construction of a current-fingerprint record. Legacy trial IDs are audit "
            "metadata only: prompt identity is matched against every current Primary "
            "trial for the same language. E is never migrated."
        ),
        "records": audited,
    }
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
