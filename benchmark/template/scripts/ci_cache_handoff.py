#!/usr/bin/env python3
"""Trusted-host cache handoff helpers for split benchmark CI jobs.

The evaluated checkout stays pinned to the trigger commit. Earlier shard jobs
checkpoint certified records to the disposable benchmark branch; a later shard
extracts only benchmark/cache from that ref and merges it into the already
staged workspace. The source checkout therefore remains clean and continues to
identify the exact evaluated snapshot.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import shutil
import sys


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("benchmark_cli", HERE / "benchmark.py")
if SPEC is None or SPEC.loader is None:
    raise SystemExit("cannot load benchmark.py")
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


def merge_tree(incoming: Path, target: Path) -> dict[str, int]:
    incoming = incoming.resolve()
    target = target.resolve()
    if not incoming.is_dir():
        raise benchmark.BenchmarkError(f"incoming cache tree does not exist: {incoming}")
    copied = 0
    identical = 0
    for source in sorted(incoming.rglob("*")):
        if source.is_symlink():
            raise benchmark.BenchmarkError(f"cache handoff contains symlink: {source}")
        if source.is_dir():
            continue
        relative = source.relative_to(incoming)
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            if not destination.is_file() or destination.read_bytes() != source.read_bytes():
                raise benchmark.BenchmarkError(
                    f"cache handoff collision with different bytes: {relative}"
                )
            identical += 1
            continue
        shutil.copyfile(source, destination)
        copied += 1
    return {"copied": copied, "identical": identical}


def cmd_workspace_import(args: argparse.Namespace) -> int:
    root = Path(args.workspace).resolve()
    run_path = root / "run.json"
    if not run_path.is_file():
        raise benchmark.BenchmarkError("workspace import requires run.json")
    incoming = Path(args.cache).resolve()
    summary = merge_tree(incoming, root / "cache")
    run = benchmark.json_load(run_path)
    run["cache_tree_sha256"] = benchmark.sha256_tree(root / "cache")
    benchmark.json_dump(run_path, run)
    out = {
        "schema_version": 1,
        "ok": True,
        "cache_tree_sha256": run["cache_tree_sha256"],
        **summary,
    }
    benchmark.json_dump(root / "results" / "cache_handoff_import.json", out)
    print(benchmark.json.dumps(out, indent=2) if hasattr(benchmark, "json") else out)
    return 0


def cmd_merge(args: argparse.Namespace) -> int:
    summary = merge_tree(Path(args.incoming), Path(args.target))
    print(f"copied={summary['copied']} identical={summary['identical']}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    workspace_import = sub.add_parser(
        "workspace-import",
        help="merge a prior shard's certified cache into a staged benchmark workspace",
    )
    workspace_import.add_argument("--workspace", required=True)
    workspace_import.add_argument("--cache", required=True)
    workspace_import.set_defaults(func=cmd_workspace_import)

    merge = sub.add_parser(
        "merge",
        help="collision-safe merge of one content-addressed cache tree into another",
    )
    merge.add_argument("--incoming", required=True)
    merge.add_argument("--target", required=True)
    merge.set_defaults(func=cmd_merge)

    args = parser.parse_args()
    try:
        return int(args.func(args))
    except benchmark.BenchmarkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
