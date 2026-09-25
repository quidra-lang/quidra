#!/usr/bin/env python3
"""One-shot canonicalization of the completed Quidra benchmark cache baseline."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE_RUN = "2026-09-25-5dc8989-gh25"
EVALUATED_SHA = "5dc8989342d3920ad1a64262696757257958e0aa"
EXPECTED_OLD_RECORDS = 1219
EXPECTED_CANONICAL_RECORDS = 688

NEW_BASELINE_RECORDS = {
    "v1/llm-proficiency/quidra/b6a8e854ed2ad7f62e83a5b2444055e5681f5db3800a5476986de39b6b135f15.json":
        "proficiency-trials--quidra",
    "v1/llm-proficiency/typescript/3f8731cab70943b0fe41b5fa2b2afe6ef600721ecf066a1281582d6ac7e689be.json":
        "proficiency-trials--typescript",
    "v1/llm-proficiency/zig/116233e2ca2daea174d689ad51828fe1bd70d06b58d12698c9f12eae883e79d2.json":
        "proficiency-trials--zig",
    "v1/semantic-compression/comparability/4462a2ce3637819c31d48ad8f07ecfd3329d55b4cbc82d90cae7cf2e843dfe49.json":
        "sc-comparability",
}

OBSOLETE_CACHE_TESTS = {
    "assert_learnability_worker_core_projection_is_nonsemantic_only",
    "assert_language_quality_snapshot_migration_hash_ratchets",
    "assert_ecosystem_snapshot_recertifies_under_current_validator",
    "assert_semantic_validator_recertification_is_narrow",
    "assert_semantic_legacy_full_hash_projection_is_explicit",
    "assert_semantic_docs_projection_ignores_only_candidates",
    "assert_semantic_owner_cross_run_density_requires_exact_experiment_identity",
    "assert_semantic_recertification_requires_exact_canonical_fragments",
}


def stable_hash(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    ).hexdigest()


def validate_completed_baseline() -> dict[str, str]:
    run_root = ROOT / "benchmark" / BASELINE_RUN
    summary = json.loads((run_root / "summary.json").read_text())
    evaluations = summary.get("primary_evaluations") or {}
    expected = {
        "semantic_compression",
        "llm_learnability",
        "language_quality",
        "ecosystem",
        "llm_proficiency",
    }
    assert set(evaluations) == expected, evaluations
    assert all(
        (row or {}).get("status") == "COMPLETE"
        for row in evaluations.values()
    ), evaluations
    assert summary.get("evaluated", {}).get("commit_sha") == EVALUATED_SHA, summary

    usage = json.loads((run_root / "cache_usage.json").read_text())
    assert int(usage.get("hit_count", 0)) == 684, usage.get("hit_count")
    assert int(usage.get("miss_count", 0)) == 3, usage.get("miss_count")

    active: dict[str, str] = {}
    for uid, row in (usage.get("hits") or {}).items():
        rel = str((row or {}).get("record") or "")
        if rel:
            active.setdefault(rel, uid)
    active.update(NEW_BASELINE_RECORDS)
    assert len(active) == EXPECTED_CANONICAL_RECORDS, len(active)
    return active


def canonicalize_records(active: dict[str, str]) -> tuple[int, int]:
    cache = ROOT / "benchmark" / "cache"
    existing = sorted((cache / "v1").rglob("*.json"))
    assert len(existing) == EXPECTED_OLD_RECORDS, len(existing)
    active_paths = {Path(rel) for rel in active}

    for rel, uid in sorted(active.items()):
        path = cache / rel
        assert path.is_file(), rel
        record = json.loads(path.read_text())
        payload = record.get("fingerprint_payload")
        assert isinstance(payload, dict), rel
        assert record.get("fingerprint") == stable_hash(payload), rel
        assert record.get("result_sha256") == stable_hash(record.get("result")), rel

        if "Quidra" in (payload.get("assigned_languages") or []):
            identity = (
                (record.get("compatibility") or {})
                .get("quidra_execution_identity")
            )
            assert isinstance(identity, dict) and identity.get("sha256"), rel
            assert isinstance(identity.get("git_objects"), dict), rel

        certification = dict(record.get("certification") or {})
        for key in list(certification):
            if (
                re.search(r"(migration|recertif|snapshot|legacy)", key, re.I)
                or key == "new_paid_provider_call"
            ):
                certification.pop(key, None)
        certification.update(
            {
                "unit_complete": True,
                "validator_pass": True,
                "primary_complete": True,
                "canonical_baseline_run_id": BASELINE_RUN,
                "canonical_baseline_formal_complete": True,
            }
        )

        old_provenance = record.get("provenance") or {}
        record["certification"] = certification
        record.pop("migration", None)
        record["provenance"] = {
            "run_id": BASELINE_RUN,
            "work_unit_id": uid,
            "prompt_sha256": old_provenance.get("prompt_sha256"),
            "canonical_baseline": True,
            "evaluated_commit_sha": EVALUATED_SHA,
        }
        path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")

    removed = 0
    for path in existing:
        if path.relative_to(cache) not in active_paths:
            path.unlink()
            removed += 1

    expected_removed = EXPECTED_OLD_RECORDS - EXPECTED_CANONICAL_RECORDS
    assert removed == expected_removed, (removed, expected_removed)

    for directory in (cache / "provenance", cache / "snapshots"):
        if directory.exists():
            shutil.rmtree(directory)
    for file in (
        cache / "proficiency-recertification-audit.json",
        cache / "recertification-report.json",
        ROOT / "benchmark/template/scripts/audit_proficiency_legacy.py",
        ROOT / ".github/workflows/cache-recertify-once.yml",
        ROOT / ".github/workflows/benchmark-cache-canonicalize-once.yml",
    ):
        file.unlink(missing_ok=True)

    for directory in sorted(
        [path for path in (cache / "v1").rglob("*") if path.is_dir()],
        key=lambda path: len(path.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass

    return len(existing), removed


def simplify_cache_policy() -> None:
    path = ROOT / "benchmark/template/config/cache_policy.json"
    policy = json.loads(path.read_text())
    reuse = policy.get("reuse_conditions") or {}
    policy["reuse_conditions"] = {
        key: reuse[key]
        for key in (
            "scored_output_cap",
            "same_result_metadata_refresh",
            "llm_proficiency_runtime_prompt_identity",
        )
        if key in reuse
    }
    policy["reuse_conditions"]["quidra_execution_identity"] = (
        "Every Quidra cache record must carry an explicit exact compiler/runtime "
        "execution identity. Missing identity is a cache miss; historical baseline "
        "fallback is not supported."
    )
    policy["policy"] = (
        "Only self-contained certified records at the exact current fingerprint may "
        "be reused. Hydration never searches older fingerprints, migration snapshots, "
        "provenance archives, or historical run directories. Every exact hit is "
        "revalidated by the current validator."
    )
    policy["non_dependency_note"] = (
        "Run IDs, workflow identity, branch names, host paths and cache provenance "
        "metadata do not enter scientific identity. Exact scored prompts/contracts, "
        "assigned requirements, readable scientific inputs, model behavior identity "
        "where applicable, toolchains, mechanical validator contracts, declared "
        "epochs, and Quidra execution identity remain enforced."
    )

    old_identity = policy.get("quidra_execution_identity") or {}
    policy["quidra_execution_identity"] = {
        "input_paths": old_identity.get("input_paths") or [],
        "purpose": (
            "Prevent stale same-version Quidra reuse. Current records that measure "
            "Quidra carry these Git object IDs and their aggregate hash explicitly."
        ),
    }

    epochs = policy.get("declared_epochs") or {}
    epochs["note"] = (
        "Declared epochs are explicit measurement pins. Change an epoch only when "
        "that measurement contract should intentionally be refreshed."
    )
    epochs["semantic_compression_note"] = (
        "Canonical-fragment v6 current contract. Reuse is exact-current-fingerprint only."
    )
    epochs["llm_proficiency_note"] = (
        "Complete Primary trials v6. Exact runtime-owned prompt/trial/workload identity "
        "is required."
    )
    policy["declared_epochs"] = epochs
    path.write_text(json.dumps(policy, indent=2) + "\n")


def patch_exact_only_hydration() -> None:
    path = ROOT / "benchmark/template/scripts/benchmark.py"
    text = path.read_text()

    fallback_start = text.index(
        "        else:\n"
        "            compatible_path, record, compatibility_problem = (\n"
        "                find_primary_projection_compatible_cache_record("
    )
    fallback_end = text.index(
        "        migration_problem = cache_record_migration_problem(root, record)\n",
        fallback_start,
    )
    exact_miss = (
        "        else:\n"
        "            record_miss(\n"
        "                uid,\n"
        "                fingerprint,\n"
        "                unit,\n"
        '                "no exact certified record",\n'
        "                invalidated=False,\n"
        "            )\n"
        "            continue\n"
    )
    text = text[:fallback_start] + exact_miss + text[fallback_end:]

    migration_check_start = text.index(
        "        migration_problem = cache_record_migration_problem(root, record)\n"
    )
    migration_check_end = text.index(
        "        cap_problem = cache_cap_reuse_problem(root, record, unit)\n",
        migration_check_start,
    )
    text = text[:migration_check_start] + text[migration_check_end:]
    text = text.replace(
        "        migration_metadata = recover_current_migration_metadata(root, record)\n",
        "        migration_metadata = None\n",
        1,
    )

    legacy_identity_start = text.index("def _legacy_cache_run_commit_prefix(")
    legacy_identity_end = text.index("def quidra_target_identity(", legacy_identity_start)
    identity_guard = (
        "def cache_quidra_execution_reuse_problem(\n"
        "    root: Path, record: dict[str, Any]\n"
        ") -> str | None:\n"
        '    """Require exact explicit Quidra compiler/runtime execution identity."""\n'
        '    payload = record.get("fingerprint_payload") or {}\n'
        '    target = str(cache_policy(root).get("target_language") or "Quidra")\n'
        '    if target not in (payload.get("assigned_languages") or []):\n'
        "        return None\n"
        "    current = current_quidra_execution_identity(root)\n"
        "    if current is None:\n"
        "        if lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):\n"
        '            return "current run is missing the trusted Quidra execution identity"\n'
        "        return None\n"
        "    recorded = (record.get(\"compatibility\") or {}).get(\"quidra_execution_identity\")\n"
        "    if not isinstance(recorded, dict):\n"
        '        return "cached Quidra record has no explicit execution identity"\n'
        '    if recorded.get("sha256") != current.get("sha256"):\n'
        '        return "Quidra compiler/runtime implementation changed"\n'
        '    if recorded.get("git_objects") != current.get("git_objects"):\n'
        '        return "Quidra execution-input object map changed"\n'
        "    return None\n\n\n"
    )
    text = text[:legacy_identity_start] + identity_guard + text[legacy_identity_end:]

    parser_start = text.index(
        '    bind_lq = sub.add_parser(\n'
        '        "cache-bind-language-quality-snapshot",'
    )
    parser_end = text.index(
        '    impact = sub.add_parser(\n'
        '        "cache-impact",',
        parser_start,
    )
    text = text[:parser_start] + text[parser_end:]
    path.write_text(text)


def remove_named_top_level_functions(path: Path, names: set[str]) -> None:
    text = path.read_text()
    tree = ast.parse(text)
    lines = text.splitlines(keepends=True)
    targets = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in targets}
    missing = names - found
    assert not missing, f"missing obsolete tests: {sorted(missing)}"

    for node in sorted(targets, key=lambda item: item.lineno, reverse=True):
        start = node.lineno - 1
        end = node.end_lineno or node.lineno
        while end < len(lines) and not lines[end].strip():
            end += 1
        del lines[start:end]
    out = "".join(lines)
    for name in names:
        out = re.sub(
            rf"(?m)^    {re.escape(name)}\(\)\n",
            "",
            out,
        )
    path.write_text(out)


def prune_unreachable_legacy_helpers() -> list[str]:
    path = ROOT / "benchmark/template/scripts/benchmark.py"
    text = path.read_text()
    tree = ast.parse(text)
    defs = {
        node.name: node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    pattern = re.compile(r"(legacy|recertif|migration)", re.I)
    candidates = {name for name in defs if pattern.search(name)}
    if not candidates:
        return []

    def names_in(node: ast.AST) -> set[str]:
        return {
            child.id
            for child in ast.walk(node)
            if isinstance(child, ast.Name)
        }

    externally_referenced: set[str] = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in candidates:
            continue
        externally_referenced |= names_in(node) & candidates

    keep = set(externally_referenced)
    changed = True
    while changed:
        changed = False
        for name in list(keep):
            refs = names_in(defs[name]) & candidates
            before = len(keep)
            keep |= refs
            changed |= len(keep) != before

    removable = candidates - keep
    if not removable:
        return []

    lines = text.splitlines(keepends=True)
    for name in sorted(
        removable,
        key=lambda item: defs[item].lineno,
        reverse=True,
    ):
        node = defs[name]
        start = node.lineno - 1
        end = node.end_lineno or node.lineno
        while end < len(lines) and not lines[end].strip():
            end += 1
        del lines[start:end]
    path.write_text("".join(lines))
    return sorted(removable)


def write_baseline_metadata(active: dict[str, str], old_total: int, removed: int) -> None:
    cache = ROOT / "benchmark/cache"
    baseline = {
        "schema_version": 1,
        "baseline_run_id": BASELINE_RUN,
        "evaluated_commit_sha": EVALUATED_SHA,
        "formal_complete": True,
        "record_count": EXPECTED_CANONICAL_RECORDS,
        "records_sha256": hashlib.sha256(
            ("\n".join(sorted(active)) + "\n").encode()
        ).hexdigest(),
        "cache_policy": "exact-current-fingerprint-only",
        "historical_fallback": False,
        "pre_compaction_record_count": old_total,
        "removed_obsolete_record_count": removed,
    }
    (cache / "baseline.json").write_text(
        json.dumps(baseline, indent=2, sort_keys=True) + "\n"
    )
    (cache / "README.md").write_text(
        "\n".join(
            [
                "# Certified benchmark cache",
                "",
                "This directory is the complete reusable measurement state for the next Quidra benchmark.",
                "",
                f"The canonical baseline is {BASELINE_RUN}. It completed all five Primary evaluations.",
                f"The cache is compacted to the {EXPECTED_CANONICAL_RECORDS} certified records required by that completed contract.",
                "",
                "A record is a HIT only at its exact current fingerprint. Hydration does not search",
                "older fingerprints, historical run directories, migration snapshots, provenance",
                "archives, or recertification sources. Every HIT is revalidated by the current",
                "validator. Quidra records additionally carry an explicit compiler/runtime execution identity.",
                "",
                "The cache is self-contained: pre-baseline cache history is not required for reuse.",
                "A future scientific input change creates a MISS only for the affected fingerprint.",
                "",
            ]
        )
    )


def apply() -> None:
    active = validate_completed_baseline()
    old_total, removed = canonicalize_records(active)
    simplify_cache_policy()
    patch_exact_only_hydration()
    remove_named_top_level_functions(
        ROOT / "tests/benchmark_cache_tests.py",
        OBSOLETE_CACHE_TESTS,
    )
    pruned = prune_unreachable_legacy_helpers()
    write_baseline_metadata(active, old_total, removed)

    assert len(list((ROOT / "benchmark/cache/v1").rglob("*.json"))) == EXPECTED_CANONICAL_RECORDS
    assert not (ROOT / "benchmark/cache/provenance").exists()
    assert not (ROOT / "benchmark/cache/snapshots").exists()
    print(
        json.dumps(
            {
                "baseline": BASELINE_RUN,
                "canonical_records": EXPECTED_CANONICAL_RECORDS,
                "removed_cache_records": removed,
                "removed_obsolete_tests": sorted(OBSOLETE_CACHE_TESTS),
                "pruned_unreachable_legacy_helpers": pruned,
            },
            indent=2,
            sort_keys=True,
        )
    )


def finalize() -> None:
    ci = ROOT / ".github/workflows/ci.yml"
    marker = "\n  # BEGIN ONE-SHOT BENCHMARK CACHE CLEANUP\n"
    text = ci.read_text()
    assert marker in text, "temporary CI cleanup job marker is missing"
    ci.write_text(text.split(marker, 1)[0].rstrip() + "\n")
    (ROOT / ".github/workflows/benchmark-cache-canonicalize-once.yml").unlink(
        missing_ok=True
    )
    Path(__file__).unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("apply", "finalize"))
    args = parser.parse_args()
    if args.mode == "apply":
        apply()
    else:
        finalize()


if __name__ == "__main__":
    main()
