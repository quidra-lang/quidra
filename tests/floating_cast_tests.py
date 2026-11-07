#!/usr/bin/env python3
"""Check floating casts with exact gradients and explicit rejection boundaries."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    fixtures = Path(__file__).with_name("floating_casts")
    cases = json.loads((fixtures / "cases.json").read_text())
    with tempfile.TemporaryDirectory(prefix="quidra-floating-casts-") as scratch:
        env = os.environ.copy()
        env["QUIDRA_CACHE_DIR"] = str(Path(scratch) / "cache")
        for case in cases:
            expected = case["expected"]
            for mode in case["saved_modes"]:
                env["QUIDRA_SAVED_TENSORS"] = mode
                phase = expected.get("phase", "run")
                print(f"floating cast: {case['source']} ({mode}, {phase})", flush=True)
                result = subprocess.run(
                    [compiler, phase, str(fixtures / case["source"])],
                    env=env, capture_output=True, text=True, timeout=90,
                )
                valid_exit = (result.returncode != 0 if expected["rc"] == "nonzero"
                              else result.returncode == expected["rc"])
                valid_output = ("stdout" not in expected or result.stdout == expected["stdout"])
                valid_diagnostic = all(expected[key] in result.stderr for key in
                                       ("diagnostic_code", "message") if key in expected)
                if not (valid_exit and valid_output and valid_diagnostic):
                    print(f"{case['source']} ({mode}): exit {result.returncode}", file=sys.stderr)
                    print(result.stdout, file=sys.stderr, end="")
                    print(result.stderr, file=sys.stderr, end="")
                    return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
