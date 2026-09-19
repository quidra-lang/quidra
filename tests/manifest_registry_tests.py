#!/usr/bin/env python3
"""Cross-check quidra.manifest.json against the compiler's own builtin registry.

`quidra describe` prints the manifest verbatim, so nothing else proves the manifest
still matches the compiler. This walks the registry in include/quidra/language.hpp
and rejects any bare builtin the manifest claims but the compiler does not define,
or that the compiler defines but the manifest omits.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

QUIDRA = Path(sys.argv[1]).resolve()
ROOT = Path(sys.argv[2]).resolve()

manifest = json.loads((ROOT / "quidra.manifest.json").read_text(encoding="utf-8"))
header = (ROOT / "include/quidra/language.hpp").read_text(encoding="utf-8")

# builtin_callables holds the bare names callable without a namespace.
block = re.search(
    r"builtin_callables\{\{(.*?)\}\};", header, re.S)
if not block:
    raise SystemExit("could not locate builtin_callables in language.hpp")
registry = set(re.findall(r'\{"([A-Za-z_][A-Za-z_0-9]*)"', block.group(1)))
if not registry:
    raise SystemExit("builtin_callables parsed as empty; the header shape changed")

# The manifest also lists spellings that are types or literal escapes rather than
# callables in that registry; those are checked by compiling them instead.
declared = set(manifest["current_builtins"])
non_callable = {
    "error", "enter", "tab", "home", "quote", "backspace", "page", "vtab", "bell",
}
claimed_callables = declared - non_callable

failures = []

missing = sorted(registry - claimed_callables)
if missing:
    failures.append(
        f"compiler defines bare builtins the manifest omits: {', '.join(missing)}")

invented = sorted(claimed_callables - registry)
if invented:
    failures.append(
        f"manifest claims bare builtins the compiler does not define: {', '.join(invented)}")

# Each claimed bare builtin must resolve in call position. The call itself may be
# rejected for its arguments; only an unknown name means the manifest invented it.
with tempfile.TemporaryDirectory() as raw:
    tmp = Path(raw)
    for name in sorted(claimed_callables):
        source = tmp / "probe.qui"
        source.write_text(f"{name}()\n", encoding="utf-8")
        result = subprocess.run(
            [str(QUIDRA), "check", str(source)],
            cwd=tmp, text=True, capture_output=True, check=False)
        if "UNKNOWN_NAME" in (result.stdout + result.stderr):
            failures.append(
                f"manifest lists '{name}' but the compiler reports it as an unknown name")

if failures:
    print("\n".join(failures), file=sys.stderr)
    print(f"manifest registry: {len(failures)} mismatch(es)", file=sys.stderr)
    raise SystemExit(1)

print(f"manifest registry: {len(claimed_callables)} bare builtins match the compiler")
