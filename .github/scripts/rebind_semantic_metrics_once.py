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

def runner(source: Path) -> Path:
    return source / "benchmark/template/scripts/benchmark.py"

def run_cli(source: Path, *args: str) -> None:
    subprocess.run(
        [sys.executable, str(runner(source)), *args],
        cwd=source,
        check=True,
    )

def record_index(source: Path) -> dict[str, list[tuple[Path, dict]]]:
    root = source / "benchmark/cache/v1"
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
    index = record_index(source)
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
        result_path = agent_dir / "result.json"
        dump(result_path, result)

        run_cli(
            source,
            "result-check",
            "--workspace", str(workspace),
            "--id", agent_id,
        )
        run_cli(
            source,
            "ledger-update",
            "--workspace", str(workspace),
            "--id", uid,
            "--status", "RUNNING",
        )
        cmd = [
            "ledger-update",
            "--workspace", str(workspace),
            "--id", uid,
            "--status", "COMPLETE",
            "--validation-result", "PASS",
        ]
        for evidence in (unit.get("evidence_paths") or []):
            cmd.extend(["--evidence", str(evidence)])
        run_cli(source, *cmd)

        plan.append({
            "work_unit_id": uid,
            "old_path": old_path.relative_to(
                source / "benchmark/cache"
            ).as_posix(),
            "old_result_sha256": record["result_sha256"],
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

        new_record = load(new_path)
        new_result = new_record.get("result")
        if not isinstance(new_result, dict):
            raise SystemExit(f"{uid}: promoted result is missing")
        digest = result_sha(new_result)
        if digest != old.get("old_result_sha256"):
            raise SystemExit(f"{uid}: post-cleanup rebind changed scientific result")
        if new_record.get("result_sha256") != digest:
            raise SystemExit(f"{uid}: promoted result self-integrity failed")
        cert = new_record.get("certification") or {}
        if cert.get("unit_complete") is not True or cert.get("validator_pass") is not True:
            raise SystemExit(f"{uid}: promoted record is not COMPLETE+PASS")

        cert.update({
            "canonical_baseline_run_id": BASELINE,
            "canonical_baseline_formal_complete": True,
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

    args = parser.parse_args()
    source = Path(args.source_repo).resolve()
    if args.command == "stage":
        stage(source, Path(args.workspace).resolve(), Path(args.output).resolve())
    else:
        prune(source, Path(args.plan).resolve(), Path(args.promotion).resolve())
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
