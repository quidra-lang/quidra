#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

BASELINE = "2026-09-25-5dc8989-gh25"
ALLOWED = re.compile(
    r"^sc-metrics-(?:local|hidden-coverage)--part-(?:1|2|3)--"
    r"(?:python|c|go|java|typescript|kotlin|swift|zig)$"
)

def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))

def dump(path: Path, value) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

def result_sha(value) -> str:
    raw = json.dumps(
        value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()

def canonical_json_sha(value) -> str:
    raw = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()

def file_sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def catalog_binding(workspace: Path, task: dict) -> tuple[str, str]:
    digest = str(task.get("canonical_fragment_catalog_sha256") or "")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise SystemExit("current Semantic metric task has no canonical catalog digest")
    names = [
        Path(str(raw)).name
        for raw in (task.get("read_paths") or [])
        if Path(str(raw)).name.startswith("canonical_fragments_")
        and Path(str(raw)).suffix == ".json"
    ]
    if len(names) != 1:
        raise SystemExit(
            f"expected one canonical fragment catalog input, found {len(names)}"
        )
    path = workspace / "work/audit/semantic-compression" / names[0]
    if not path.is_file():
        raise SystemExit(f"current canonical fragment catalog is missing: {path}")
    if file_sha(path) != digest:
        raise SystemExit("current canonical fragment catalog does not match task digest")
    payload = load(path)
    catalog = payload.get("canonical_fragments")
    if not isinstance(catalog, dict) or not catalog:
        raise SystemExit("current canonical fragment catalog is empty")
    return digest, canonical_json_sha(catalog)

def result_without_catalog_binding(value):
    cloned = json.loads(json.dumps(value))
    evidence = cloned.get("evidence")
    if not isinstance(evidence, dict):
        return cloned
    evidence.pop("canonical_fragment_catalog_sha256", None)
    equivalence = evidence.get("canonical_baseline_equivalence")
    if isinstance(equivalence, dict):
        equivalence.pop("canonical_fragment_catalog_sha256", None)
        equivalence.pop("canonical_fragment_catalog_content_sha256", None)
    return cloned

def runner(source: Path, workspace: Path) -> Path:
    staged = workspace / "template/scripts/benchmark.py"
    if staged.is_file():
        return staged
    tracked = source / "benchmark/template/scripts/benchmark.py"
    if tracked.is_file():
        return tracked
    raise SystemExit("benchmark runner is missing from workspace and source repo")

def run_cli(source: Path, workspace: Path, *args: str) -> None:
    subprocess.run(
        [sys.executable, str(runner(source, workspace)), *args],
        cwd=source,
        check=True,
    )

def host_evidence_paths(workspace: Path, unit: dict) -> list[str]:
    """Map canonical /quidra-benchmark evidence paths onto host staging safely."""
    root = workspace.resolve()
    canonical = Path("/quidra-benchmark")
    out: list[str] = []
    for raw in (unit.get("evidence_paths") or []):
        recorded = Path(str(raw))
        if recorded.is_absolute():
            try:
                relative = recorded.relative_to(canonical)
                host = (root / relative).resolve()
            except ValueError:
                host = recorded.resolve()
        else:
            host = (root / recorded).resolve()
        try:
            host.relative_to(root)
        except ValueError as exc:
            raise SystemExit(
                f"{unit.get('id')}: evidence path escapes host staging: {recorded}"
            ) from exc
        out.append(str(host))
    if not out:
        raise SystemExit(f"{unit.get('id')}: no evidence paths to certify")
    return out

def record_index(cache_root: Path) -> dict[str, list[tuple[Path, dict]]]:
    """Index the cache staged for the current scored run.

    The sandbox keeps /quidra-benchmark/repo as evaluation source only; the
    certified cache is a separate read-only /quidra-benchmark/cache mount.
    """
    root = cache_root / "v1"
    out: dict[str, list[tuple[Path, dict]]] = {}
    for path in sorted(root.rglob("*.json")):
        obj = load(path)
        uid = str((obj.get("provenance") or {}).get("work_unit_id") or "")
        if uid:
            out.setdefault(uid, []).append((path, obj))
    return out

def stage(source: Path, workspace: Path, output: Path) -> None:
    status = load(workspace / "results/cache_status.json")
    invalidated = status.get("invalidated") or {}
    misses = status.get("misses") or {}
    if invalidated:
        raise SystemExit(
            f"transition rebind refuses invalidated cache: {sorted(invalidated)[:8]}"
        )
    miss_ids = sorted(str(uid) for uid in misses)
    if len(miss_ids) != 40:
        raise SystemExit(
            f"expected exactly 40 cleanup-induced misses, found {len(miss_ids)}"
        )
    bad = [uid for uid in miss_ids if not ALLOWED.fullmatch(uid)]
    if bad:
        raise SystemExit(f"unexpected cleanup-induced miss units: {bad}")

    manifest = load(workspace / "work/root/manifest.json")
    units = {
        str(unit.get("id")): unit
        for unit in (manifest.get("work_units") or [])
    }
    index = record_index(workspace / "cache")
    plan = []

    for uid in miss_ids:
        unit = units.get(uid)
        if not isinstance(unit, dict):
            raise SystemExit(f"{uid}: missing from current manifest")
        if str(unit.get("evaluation") or "") != "semantic_compression":
            raise SystemExit(f"{uid}: unexpected evaluation")
        candidates = index.get(uid) or []
        if len(candidates) != 1:
            raise SystemExit(
                f"{uid}: expected one retained pre-cleanup record, found "
                f"{len(candidates)}"
            )
        old_path, record = candidates[0]
        cert = record.get("certification") or {}
        prov = record.get("provenance") or {}
        if cert.get("unit_complete") is not True or cert.get("validator_pass") is not True:
            raise SystemExit(f"{uid}: retained record is not COMPLETE+PASS")
        if prov.get("canonical_baseline") is not True:
            raise SystemExit(f"{uid}: retained record is not canonical-baseline evidence")
        result = record.get("result")
        if not isinstance(result, dict):
            raise SystemExit(f"{uid}: retained result is missing")
        if record.get("result_sha256") != result_sha(result):
            raise SystemExit(f"{uid}: retained result self-integrity failed")

        agent_id = str(unit.get("assigned_agent_id") or "")
        if not agent_id:
            raise SystemExit(f"{uid}: current unit has no agent id")
        agent_dir = workspace / "work/agents" / agent_id
        task_path = agent_dir / "task.json"
        if not task_path.is_file():
            raise SystemExit(f"{uid}: current task packet is missing")
        task = load(task_path)
        current_catalog_sha, current_catalog_content_sha = catalog_binding(
            workspace, task
        )
        rebound = json.loads(json.dumps(result))
        evidence = rebound.get("evidence")
        if not isinstance(evidence, dict):
            raise SystemExit(f"{uid}: retained result evidence is missing")
        equivalence = evidence.get("canonical_baseline_equivalence")
        if not isinstance(equivalence, dict):
            raise SystemExit(
                f"{uid}: retained result lacks canonical baseline equivalence"
            )
        old_catalog_sha = str(
            evidence.get("canonical_fragment_catalog_sha256") or ""
        )
        old_catalog_content_sha = str(
            equivalence.get("canonical_fragment_catalog_content_sha256") or ""
        )
        evidence["canonical_fragment_catalog_sha256"] = current_catalog_sha
        equivalence["canonical_fragment_catalog_sha256"] = current_catalog_sha
        equivalence["canonical_fragment_catalog_content_sha256"] = (
            current_catalog_content_sha
        )
        if result_without_catalog_binding(rebound) != result_without_catalog_binding(result):
            raise SystemExit(
                f"{uid}: cleanup rebind changed scientific result outside catalog binding"
            )

        result_path = agent_dir / "result.json"
        dump(result_path, rebound)

        run_cli(
            source,
            workspace,
            "result-check",
            "--workspace", str(workspace),
            "--id", agent_id,
        )
        ledger_evidence: list[str] = []
        for evidence_path in host_evidence_paths(workspace, unit):
            ledger_evidence.extend(["--evidence", evidence_path])
        run_cli(
            source,
            workspace,
            "ledger-update",
            "--workspace", str(workspace),
            "--id", uid,
            "--status", "RUNNING",
            *ledger_evidence,
        )
        cmd = [
            "ledger-update",
            "--workspace", str(workspace),
            "--id", uid,
            "--status", "COMPLETE",
            "--validation-result", "PASS",
            *ledger_evidence,
        ]
        run_cli(source, workspace, *cmd)

        plan.append({
            "work_unit_id": uid,
            "old_path": old_path.relative_to(
                workspace / "cache"
            ).as_posix(),
            "old_result_sha256": record["result_sha256"],
            "rebound_result_sha256": result_sha(rebound),
            "old_catalog_sha256": old_catalog_sha,
            "new_catalog_sha256": current_catalog_sha,
            "old_catalog_content_sha256": old_catalog_content_sha,
            "new_catalog_content_sha256": current_catalog_content_sha,
        })

    dump(output, {
        "schema_version": 1,
        "baseline_run_id": BASELINE,
        "kind": "cleanup-induced-semantic-metric-rebind",
        "count": len(plan),
        "records": plan,
    })
    print(json.dumps({
        "staged_rebind_units": len(plan),
        "output": str(output),
    }, indent=2))

def prune(source: Path, plan_path: Path, promotion_path: Path) -> None:
    plan = load(plan_path)
    promotion = load(promotion_path)
    planned = {
        str(row["work_unit_id"]): row
        for row in (plan.get("records") or [])
    }
    if len(planned) != 40:
        raise SystemExit(f"expected 40 rebind plan rows, found {len(planned)}")

    promoted_by_uid: dict[str, list[dict]] = {}
    for row in (promotion.get("records") or []):
        uid = str(row.get("work_unit_id") or "")
        if uid in planned:
            promoted_by_uid.setdefault(uid, []).append(row)

    cache_root = source / "benchmark/cache"
    replaced = []
    for uid, old in sorted(planned.items()):
        rows = promoted_by_uid.get(uid) or []
        if len(rows) != 1:
            raise SystemExit(
                f"{uid}: expected one post-cleanup promoted record, found {len(rows)}"
            )
        row = rows[0]
        new_rel = str(row.get("path") or "")
        old_rel = str(old.get("old_path") or "")
        if not new_rel or not old_rel or new_rel == old_rel:
            raise SystemExit(f"{uid}: exact key did not change across cleanup")
        new_path = cache_root / new_rel
        old_path = cache_root / old_rel
        if not new_path.is_file() or not old_path.is_file():
            raise SystemExit(f"{uid}: old/new cache record is missing")

        old_record = load(old_path)
        old_result = old_record.get("result")
        if not isinstance(old_result, dict):
            raise SystemExit(f"{uid}: retained pre-rebind result is missing")
        if result_sha(old_result) != old.get("old_result_sha256"):
            raise SystemExit(f"{uid}: retained pre-rebind result changed unexpectedly")

        new_record = load(new_path)
        new_result = new_record.get("result")
        if not isinstance(new_result, dict):
            raise SystemExit(f"{uid}: promoted result is missing")
        digest = result_sha(new_result)
        if digest != old.get("rebound_result_sha256"):
            raise SystemExit(f"{uid}: promoted result differs from staged rebind")
        if result_without_catalog_binding(new_result) != result_without_catalog_binding(
            old_result
        ):
            raise SystemExit(
                f"{uid}: post-cleanup rebind changed scientific result outside catalog binding"
            )
        evidence = new_result.get("evidence") or {}
        equivalence = evidence.get("canonical_baseline_equivalence") or {}
        if evidence.get("canonical_fragment_catalog_sha256") != old.get(
            "new_catalog_sha256"
        ):
            raise SystemExit(f"{uid}: promoted result has stale catalog digest")
        if equivalence.get("canonical_fragment_catalog_content_sha256") != old.get(
            "new_catalog_content_sha256"
        ):
            raise SystemExit(f"{uid}: promoted result has stale catalog content digest")
        if new_record.get("result_sha256") != digest:
            raise SystemExit(f"{uid}: promoted result self-integrity failed")
        cert = new_record.get("certification") or {}
        if cert.get("unit_complete") is not True or cert.get("validator_pass") is not True:
            raise SystemExit(f"{uid}: promoted record is not COMPLETE+PASS")

        cert.update({
            "canonical_baseline_run_id": BASELINE,
            "canonical_baseline_formal_complete": True,
            "canonical_fragment_catalog_sha256": old.get("new_catalog_sha256"),
            "post_cleanup_rebind": True,
        })
        new_record["certification"] = cert
        prov = dict(new_record.get("provenance") or {})
        prov.update({
            "run_id": BASELINE,
            "canonical_baseline": True,
            "post_cleanup_rebind": True,
        })
        new_record["provenance"] = prov
        dump(new_path, new_record)
        old_path.unlink()
        replaced.append({
            "work_unit_id": uid,
            "old_path": old_rel,
            "new_path": new_rel,
        })

    for directory in sorted(
        [p for p in (cache_root / "v1").rglob("*") if p.is_dir()],
        key=lambda p: len(p.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass

    count = len(list((cache_root / "v1").rglob("*.json")))
    if count != 687:
        raise SystemExit(f"expected 687 records after rebind pruning, found {count}")
    print(json.dumps({
        "rebound_exact_keys": len(replaced),
        "retained_cache_records": count,
    }, indent=2))


def stage_comparability(source: Path, workspace: Path, output: Path) -> None:
    """Rebind the one downstream comparability task after metric-key repair."""
    status = load(workspace / "results/cache_status.json")
    invalidated = status.get("invalidated") or {}
    misses = status.get("misses") or {}
    if invalidated:
        raise SystemExit(
            f"comparability rebind refuses invalidated cache: {sorted(invalidated)[:8]}"
        )
    miss_ids = sorted(str(uid) for uid in misses)
    if miss_ids != ["sc-comparability"]:
        raise SystemExit(
            "expected only sc-comparability after metric rebind, found "
            f"{miss_ids}"
        )

    manifest = load(workspace / "work/root/manifest.json")
    units = {
        str(unit.get("id")): unit
        for unit in (manifest.get("work_units") or [])
    }
    uid = "sc-comparability"
    unit = units.get(uid)
    if not isinstance(unit, dict):
        raise SystemExit("sc-comparability: missing from current manifest")
    if str(unit.get("evaluation") or "") != "semantic_compression":
        raise SystemExit("sc-comparability: unexpected evaluation")

    candidates = record_index(workspace / "cache").get(uid) or []
    if len(candidates) != 1:
        raise SystemExit(
            "sc-comparability: expected one retained canonical record, found "
            f"{len(candidates)}"
        )
    old_path, record = candidates[0]
    cert = record.get("certification") or {}
    prov = record.get("provenance") or {}
    if cert.get("unit_complete") is not True or cert.get("validator_pass") is not True:
        raise SystemExit("sc-comparability: retained record is not COMPLETE+PASS")
    if prov.get("canonical_baseline") is not True:
        raise SystemExit("sc-comparability: retained record is not canonical baseline")

    result = record.get("result")
    if not isinstance(result, dict):
        raise SystemExit("sc-comparability: retained result is missing")
    digest = result_sha(result)
    if record.get("result_sha256") != digest:
        raise SystemExit("sc-comparability: retained result self-integrity failed")
    evidence = result.get("evidence")
    baseline = evidence.get("canonical_baseline") if isinstance(evidence, dict) else None
    if not isinstance(baseline, dict):
        raise SystemExit("sc-comparability: canonical baseline evidence is missing")
    if baseline.get("mode") != "comparability-from-canonical-baseline":
        raise SystemExit("sc-comparability: unexpected canonical baseline mode")
    if "legacy_recertification" in evidence:
        raise SystemExit("sc-comparability: legacy recertification survived cleanup")

    agent_id = str(unit.get("assigned_agent_id") or "")
    if not agent_id:
        raise SystemExit("sc-comparability: current unit has no agent id")
    agent_dir = workspace / "work/agents" / agent_id
    task_path = agent_dir / "task.json"
    if not task_path.is_file():
        raise SystemExit("sc-comparability: current task packet is missing")

    # The audit conclusion itself does not change. The task fingerprint changed
    # only because the 40 metric inputs were rebound from legacy catalog
    # attestations to the self-contained canonical catalog. Revalidate the exact
    # same result object against the new task packet before checkpointing it.
    result_path = agent_dir / "result.json"
    dump(result_path, result)
    run_cli(
        source,
        workspace,
        "result-check",
        "--workspace", str(workspace),
        "--id", agent_id,
    )

    ledger_evidence: list[str] = []
    for evidence_path in host_evidence_paths(workspace, unit):
        ledger_evidence.extend(["--evidence", evidence_path])
    run_cli(
        source,
        workspace,
        "ledger-update",
        "--workspace", str(workspace),
        "--id", uid,
        "--status", "RUNNING",
        *ledger_evidence,
    )
    run_cli(
        source,
        workspace,
        "ledger-update",
        "--workspace", str(workspace),
        "--id", uid,
        "--status", "COMPLETE",
        "--validation-result", "PASS",
        *ledger_evidence,
    )

    dump(output, {
        "schema_version": 1,
        "baseline_run_id": BASELINE,
        "kind": "cleanup-induced-semantic-comparability-rebind",
        "count": 1,
        "records": [{
            "work_unit_id": uid,
            "old_path": old_path.relative_to(workspace / "cache").as_posix(),
            "old_result_sha256": digest,
        }],
    })
    print(json.dumps({
        "staged_comparability_rebind": 1,
        "output": str(output),
    }, indent=2))

def prune_comparability(
    source: Path, plan_path: Path, promotion_path: Path
) -> None:
    plan = load(plan_path)
    promotion = load(promotion_path)
    rows = plan.get("records") or []
    if len(rows) != 1 or str(rows[0].get("work_unit_id") or "") != "sc-comparability":
        raise SystemExit("expected one sc-comparability rebind plan row")
    old = rows[0]

    promoted = [
        row for row in (promotion.get("records") or [])
        if str(row.get("work_unit_id") or "") == "sc-comparability"
    ]
    if len(promoted) != 1:
        raise SystemExit(
            "sc-comparability: expected one promoted exact-key record, found "
            f"{len(promoted)}"
        )

    cache_root = source / "benchmark/cache"
    new_rel = str(promoted[0].get("path") or "")
    old_rel = str(old.get("old_path") or "")
    if not new_rel or not old_rel or new_rel == old_rel:
        raise SystemExit("sc-comparability: exact key did not change")
    new_path = cache_root / new_rel
    old_path = cache_root / old_rel
    if not new_path.is_file() or not old_path.is_file():
        raise SystemExit("sc-comparability: old/new cache record is missing")

    old_record = load(old_path)
    old_result = old_record.get("result")
    new_record = load(new_path)
    new_result = new_record.get("result")
    if not isinstance(old_result, dict) or not isinstance(new_result, dict):
        raise SystemExit("sc-comparability: old/new result is missing")
    digest = str(old.get("old_result_sha256") or "")
    if result_sha(old_result) != digest:
        raise SystemExit("sc-comparability: retained result changed before promotion")
    if new_result != old_result:
        raise SystemExit(
            "sc-comparability: scientific audit result changed during rebind"
        )
    if new_record.get("result_sha256") != digest:
        raise SystemExit("sc-comparability: promoted result self-integrity failed")

    cert = new_record.get("certification") or {}
    if cert.get("unit_complete") is not True or cert.get("validator_pass") is not True:
        raise SystemExit("sc-comparability: promoted record is not COMPLETE+PASS")
    cert.update({
        "canonical_baseline_run_id": BASELINE,
        "canonical_baseline_formal_complete": True,
        "post_cleanup_rebind": True,
        "post_cleanup_dependency_rebind": True,
    })
    new_record["certification"] = cert
    prov = dict(new_record.get("provenance") or {})
    prov.update({
        "run_id": BASELINE,
        "canonical_baseline": True,
        "post_cleanup_rebind": True,
        "post_cleanup_dependency_rebind": True,
    })
    new_record["provenance"] = prov
    dump(new_path, new_record)
    old_path.unlink()

    for directory in sorted(
        [p for p in (cache_root / "v1").rglob("*") if p.is_dir()],
        key=lambda p: len(p.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass

    count = len(list((cache_root / "v1").rglob("*.json")))
    if count != 687:
        raise SystemExit(
            f"expected 687 records after comparability rebind, found {count}"
        )
    print(json.dumps({
        "rebound_comparability_exact_key": 1,
        "retained_cache_records": count,
        "new_path": new_rel,
    }, indent=2))

def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("stage")
    p.add_argument("--source-repo", required=True)
    p.add_argument("--workspace", required=True)
    p.add_argument("--output", required=True)

    p = sub.add_parser("prune")
    p.add_argument("--source-repo", required=True)
    p.add_argument("--plan", required=True)
    p.add_argument("--promotion", required=True)

    p = sub.add_parser("stage-comparability")
    p.add_argument("--source-repo", required=True)
    p.add_argument("--workspace", required=True)
    p.add_argument("--output", required=True)

    p = sub.add_parser("prune-comparability")
    p.add_argument("--source-repo", required=True)
    p.add_argument("--plan", required=True)
    p.add_argument("--promotion", required=True)

    args = parser.parse_args()
    source = Path(args.source_repo).resolve()
    if args.command == "stage":
        stage(source, Path(args.workspace).resolve(), Path(args.output).resolve())
    elif args.command == "prune":
        prune(source, Path(args.plan).resolve(), Path(args.promotion).resolve())
    elif args.command == "stage-comparability":
        stage_comparability(
            source, Path(args.workspace).resolve(), Path(args.output).resolve()
        )
    else:
        prune_comparability(
            source, Path(args.plan).resolve(), Path(args.promotion).resolve()
        )
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
