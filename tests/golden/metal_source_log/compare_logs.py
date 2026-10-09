#!/usr/bin/env python3
"""Compare two Metal source logs (metal_source_log.mm) as multisets.

  compare_logs.py BASE_LOG HEAD_LOG

The order of compilations may differ (threads, lazy pipelines); the set of
sources and how often each is compiled may not. Exit status: 0 when the
logs are equal, 1 when a source occurs in one log only, 3 when both logs
have the same sources but compile some of them a different number of
times (ir_golden.sh metal-sources then runs both sides again).
"""

from __future__ import annotations

import hashlib
import sys
from collections import Counter
from pathlib import Path


def records(path: Path) -> Counter:
    data = path.read_bytes() if path.exists() else b""
    result: Counter = Counter()
    index = 0
    while index < len(data):
        end = data.index(b"\n", index)
        header = data[index:end].decode()
        if not header.startswith("=== "):
            raise SystemExit(f"compare_logs.py: malformed record header in {path}: {header!r}")
        length = int(header[4:])
        start = end + 1
        result[data[start:start + length]] += 1
        index = start + length + 1
    return result


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    base, head = records(Path(sys.argv[1])), records(Path(sys.argv[2]))
    if base == head:
        print(f"compare_logs.py: identical ({sum(base.values())} compilations, "
              f"{len(base)} distinct sources)")
        return 0
    for source in sorted(set(base) | set(head), key=lambda s: hashlib.sha256(s).hexdigest()):
        if base[source] != head[source]:
            digest = hashlib.sha256(source).hexdigest()[:16]
            print(f"source {digest} ({len(source)} bytes): base {base[source]}x, head {head[source]}x")
            print("    " + source[:200].decode(errors="replace").replace("\n", "\n    "))
    if set(base) == set(head):
        print("compare_logs.py: the same sources, compiled a different number of times")
        return 3
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
