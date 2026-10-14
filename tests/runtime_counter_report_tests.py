#!/usr/bin/env python3
"""Counter reports survive immediate failures and ordinary steps do no file I/O."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


probe = sys.argv[1]
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    environment = dict(os.environ)
    environment.pop("QUIDRA_COUNTERS_FLUSH", None)
    for mode, diagnostic in (("--failure", "forced counter failure"),
                             ("--handoff-failure", "PARALLEL_OWNERSHIP"),
                             ("--native-failure", "forced package failure")):
        failure = root / (mode[2:] + ".jsonl")
        environment["QUIDRA_COUNTERS"] = str(failure)
        result = subprocess.run([probe, mode], env=environment,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 101, result
        assert diagnostic in result.stderr, result.stderr
        assert failure.exists(), ("immediate failure lost buffered counters", mode)
        report = records(failure)
        assert [row["record"] for row in report] == ["step", "step", "total"], report
        assert [row["core_dispatches"] for row in report] == [3, 2, 5], report

    for flush in (None, "step"):
        path = root / ("buffered.jsonl" if flush is None else "step.jsonl")
        environment["QUIDRA_COUNTERS"] = str(path)
        if flush is None:
            environment.pop("QUIDRA_COUNTERS_FLUSH", None)
        else:
            environment["QUIDRA_COUNTERS_FLUSH"] = flush
        process = subprocess.Popen([probe, "--wait"], env=environment, text=True,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        try:
            assert process.stdout.readline() == "ready\n"
            if flush is None:
                assert not path.exists(), "ordinary step wrote a report before exit"
            else:
                assert [row["record"] for row in records(path)] == ["step"]
            output, errors = process.communicate("\n", timeout=10)
            assert process.returncode == 0, (output, errors)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        report = records(path)
        assert [row["record"] for row in report] == ["step", "step", "total"], report
        assert [row["core_dispatches"] for row in report] == [3, 0, 3], report

print("counter buffered and failure reports: ok")
