#!/usr/bin/env python3
"""Assert exact failure exits: syntax/link failures must never count as success."""
import argparse
import os
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exact", "fixed", "tensor", "package"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    cases = [(args.exact, mode) for mode in (
        "zero-zero", "inf-minus-inf", "opposite-infinities", "inf-div-inf",
        "zero-times-inf", "nan-input")]
    cases += [(args.fixed, mode) for mode in (
        "nan-format", "nan-fixed", "nan-significant", "nan32-format", "nan32-fixed",
        "nan32-significant", "nan16-format", "nan16-fixed", "nan16-significant",
        "nan16b-format", "nan16b-fixed", "nan16b-significant")]
    cases += [(args.tensor, "inf-minus-inf"), (args.tensor, "zero-times-inf"),
              (args.package, "legacy-nan"), (args.package, "normal-nan")]
    failures = 0
    environment = dict(os.environ, QUIDRA_ERROR_FORMAT="text")
    for executable, mode in cases:
        run = subprocess.run([str(executable.resolve()), mode], text=True,
                             capture_output=True, timeout=30, env=environment)
        header = ("Quidra runtime error: internal error: NaN reached real text formatting"
                  if executable == args.fixed else "Quidra runtime error[")
        ok = run.returncode == 101 and header in run.stderr
        if executable == args.tensor or executable == args.package:
            ok = ok and run.stdout == "before"
        print(f'{"PASS" if ok else "FAIL"} {executable.name} {mode}: exit={run.returncode}')
        if not ok:
            failures += 1
            print(run.stdout + run.stderr)
    print(f"{len(cases) - failures}/{len(cases)} failure contracts passed")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
