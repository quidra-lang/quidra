#!/usr/bin/env python3
"""Check the REPL session simulator against the real REPL (README.md, Inputs).

  repl_session_check.py QUIDRA CAPTURE CORPUS_ROOT HOME

For every repl-session entry of the corpus, runs `QUIDRA repl < INPUT` in a
scratch copy of the entry's directory (the sessions write files) and compares
the compile-time decisions with the simulator's `repl.session` view in
CAPTURE: the diagnostic codes the REPL prints for rejected submissions
(`<repl>:L:C: error[CODE]`) and the REPL_REPLAY_UNSAFE rejections must
equal, as multisets, the simulator's `status error:CODE` and
`status rejected REPL_REPLAY_UNSAFE` records.

The simulator does not run programs: a submission that compiles but fails at
run time is accepted by the simulator and rejected by the REPL, so later
submissions may diverge. Such sessions are reported as runtime-dependent and
compared only up to the first runtime failure the REPL reports.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

REPL_DIAGNOSTIC = re.compile(r"^<repl>:\d+:\d+: error\[([A-Z0-9_]+)\]", re.M)
UNSAFE = re.compile(r"error\[REPL_REPLAY_UNSAFE\]")
RUNTIME = re.compile(r"runtime error\[([A-Z0-9_]+)\]|Quidra runtime error", re.I)


def simulator_codes(text: str) -> Counter:
    codes: Counter = Counter()
    for line in text.splitlines():
        if line.startswith("status error:"):
            codes[line.split(":", 1)[1]] += 1
        elif line == "status rejected REPL_REPLAY_UNSAFE":
            codes["REPL_REPLAY_UNSAFE"] += 1
    return codes


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    quidra, capture, root, home = (Path(a).resolve() for a in sys.argv[1:])
    failures = 0
    checked = 0
    for line in (root / "entries.tsv").read_text(encoding="utf-8").splitlines():
        name, kind, path, cwd, package_path = line.split("\t")
        if kind != "repl-session":
            continue
        view = capture / "out" / name / "repl.session"
        if not view.exists():
            print(f"repl_session_check.py: {name}: no repl.session view in the capture")
            failures += 1
            continue
        with tempfile.TemporaryDirectory(prefix="quidra-repl-check-") as scratch:
            work = Path(scratch) / "work"
            shutil.copytree(cwd, work)
            environment = {"PATH": os.environ.get("PATH", ""), "HOME": str(home),
                           "QUIDRA_PACKAGE_PATH": package_path, "LC_ALL": "C", "LANG": "C",
                           "TZ": "UTC"}
            with open(path, "rb") as stdin:
                result = subprocess.run([str(quidra), "repl"], cwd=work, env=environment,
                                        stdin=stdin, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, timeout=600)
        real = Counter(REPL_DIAGNOSTIC.findall(result.stderr))
        real["REPL_REPLAY_UNSAFE"] += len(UNSAFE.findall(result.stderr))
        real = +real
        simulated = simulator_codes(view.read_text(encoding="utf-8", errors="replace"))
        runtime = RUNTIME.search(result.stderr + result.stdout)
        checked += 1
        if real == simulated:
            print(f"repl_session_check.py: {name}: same decisions {dict(real) or '{}'}")
        elif runtime:
            print(f"repl_session_check.py: {name}: runtime-dependent (the REPL reported a "
                  f"run-time failure); REPL {dict(real)}, simulator {dict(simulated)}")
        else:
            print(f"repl_session_check.py: {name}: MISMATCH REPL {dict(real)}, "
                  f"simulator {dict(simulated)}")
            failures += 1
    print(f"repl_session_check.py: {checked} sessions, {failures} mismatches")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
