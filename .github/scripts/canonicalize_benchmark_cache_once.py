#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

BASE = "2026-09-25-5dc8989-gh25"
NEW = {
    "v1/llm-proficiency/quidra/b6a8e854ed2ad7f62e83a5b2444055e5681f5db3800a5476986de39b6b135f15.json":
        "proficiency-trials--quidra",
    "v1/llm-proficiency/typescript/3f8731cab70943b0fe41b5fa2b2afe6ef600721ecf066a1281582d6ac7e689be.json":
        "proficiency-trials--typescript",
    "v1/llm-proficiency/zig/116233e2ca2daea174d689ad51828fe1bd70d06b58d12698c9f12eae883e79d2.json":
        "proficiency-trials--zig",
    "v1/semantic-compression/comparability/4462a2ce3637819c31d48ad8f07ecfd3329d55b4cbc82d90cae7cf2e843dfe49.json":
        "sc-comparability",
}

root = Path.cwd()
cache = root / "benchmark/cache"
usage = json.loads((root / "benchmark" / BASE / "cache_usage.json").read_text())

active: dict[str, str] = {}
for uid, row in (usage.get("hits") or {}).items():
    rel = str((row or {}).get("record") or "")
    if rel:
        active.setdefault(rel, uid)
active.update(NEW)

if len(active) != 688:
    raise SystemExit(f"expected 688 canonical records, got {len(active)}")

existing = sorted((cache / "v1").rglob("*.json"))
if len(existing) != 1219:
    raise SystemExit(f"expected reviewed 1219-record pre-cleanup cache, got {len(existing)}")

active_paths = {Path(p) for p in active}
for rel, uid in sorted(active.items()):
    path = cache / rel
    if not path.is_file():
        raise SystemExit(f"missing canonical record: {rel}")
    record = json.loads(path.read_text())
    payload = record.get("fingerprint_payload")
    if not isinstance(payload, dict):
        raise SystemExit(f"bad payload: {rel}")
    fp = hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    ).hexdigest()
    if record.get("fingerprint") != fp:
        raise SystemExit(f"fingerprint mismatch: {rel}")
    result_sha = hashlib.sha256(
        json.dumps(record.get("result"), sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    if record.get("result_sha256") != result_sha:
        raise SystemExit(f"result digest mismatch: {rel}")

    # The completed formal run is now the direct provenance authority for the
    # current exact-key record. Migration-source files are no longer needed for
    # this record to explain where its accepted result came from.
    certification = dict(record.get("certification") or {})
    certification["unit_complete"] = True
    certification["validator_pass"] = True
    certification["primary_complete"] = True
    certification["canonical_baseline_run_id"] = BASE
    certification["canonical_baseline_formal_complete"] = True
    record["certification"] = certification
    record.pop("migration", None)
    old = record.get("provenance") or {}
    record["provenance"] = {
        "run_id": BASE,
        "work_unit_id": uid,
        "prompt_sha256": old.get("prompt_sha256"),
        "canonical_baseline": True,
    }
    path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")

removed = 0
for path in existing:
    if path.relative_to(cache) not in active_paths:
        path.unlink()
        removed += 1
if removed != 531:
    raise SystemExit(f"expected to remove 531 obsolete records, removed {removed}")

for d in sorted(
    [p for p in (cache / "v1").rglob("*") if p.is_dir()],
    key=lambda p: len(p.parts),
    reverse=True,
):
    try:
        d.rmdir()
    except OSError:
        pass

baseline = {
    "schema_version": 1,
    "baseline_run_id": BASE,
    "formal_complete": True,
    "record_count": 688,
    "records_sha256": hashlib.sha256(
        ("\n".join(sorted(active)) + "\n").encode()
    ).hexdigest(),
    "phase": "canonical-current-records",
}
(cache / "baseline.json").write_text(
    json.dumps(baseline, indent=2, sort_keys=True) + "\n"
)

print(json.dumps({
    "canonical_records": 688,
    "deleted_obsolete_records": removed,
}, indent=2))
