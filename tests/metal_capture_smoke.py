#!/usr/bin/env python3
"""Native capture correctness with a fresh destination and child-only SDK flag."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="quidra-metal-capture-") as directory:
    output = Path(directory) / "capture.gputrace"
    environment = dict(os.environ)
    environment["MTL_CAPTURE_ENABLED"] = "1"
    result = subprocess.run([sys.argv[1], str(output)], env=environment,
                            capture_output=True, text=True, timeout=60)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode == 0 and not output.exists():
        raise AssertionError("successful native capture has no artifact")
    sys.exit(result.returncode)
